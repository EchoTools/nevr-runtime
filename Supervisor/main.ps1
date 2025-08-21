<#
.SYNOPSIS
    Starts, monitors, and manages multiple Echo VR dedicated server instances.
.DESCRIPTION
    This script provides a robust solution for running Echo VR servers.
    - Automatically creates a Windows Firewall rule.
    - Dynamically finds and reserves available ports to prevent conflicts.
    - Monitors each instance and restarts it if it crashes.
    - Implements a restart backoff delay to prevent crash loops.
    - Includes an optional active health check to detect hung servers.
    - Checks for new script and DLL versions on startup.
    - Cleans up all processes and files gracefully when the script is terminated.
.NOTES
    Author: Andrew Bates <a@sprock.io>
    Source: github.com/echotools
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path = "C:\echovr\ready-at-dawn-echo-arena\bin\win10\echovr.exe",
    [int]$ServerCount = 4,
    [string]$ExtraArguments = "",
    [int]$CheckIntervalSec = 5,
    [boolean]$LogToFile = $true,
    [string]$LogFolder = ".\EchoServerLogs",
    [switch]$EnableHealthChecks,
    [string]$FirewallRuleName = "EchoVR Game Server UDP Ports 6792-6810",
    [string]$FirewallPortRange = "6792-6810"
)

# --- Import Modules ---
. "$PSScriptRoot\supervisor\versioning.ps1"
. "$PSScriptRoot\supervisor\firewall.ps1"
. "$PSScriptRoot\supervisor\logging.ps1"
. "$PSScriptRoot\supervisor\ports.ps1"
. "$PSScriptRoot\supervisor\instance.ps1"

# --- Script Configuration & Versioning ---
$ScriptVersion = "1.0.0" # This line is updated by the build process
$Repo = "EchoTools/nevr-server"

# --- Initial Setup ---
Invoke-VersionAndDllChecks -Path $Path -ScriptVersion $ScriptVersion -Repo $Repo

if (-not (Test-Path -LiteralPath $Path)) {
    Write-Error "Executable not found at path: $Path"
    return
}

Ensure-FirewallRule -FirewallRuleName $FirewallRuleName -FirewallPortRange $FirewallPortRange

$logFile = ""
if ($LogToFile) {
    $logFile = Initialize-LogFile -LogFolder $LogFolder
}

# --- Main Script Logic ---
Write-LogEntry -Message "Script started. Managing $ServerCount instances." -LogToFile:$LogToFile -LogFile:$logFile
$serverInstances = @()
for ($i = 0; $i -lt $ServerCount; $i++) {
    $serverInstances += [PSCustomObject]@{
        InstanceId      = $i + 1; Process = $null; PortInfo = $null
        Status          = 'Stopped'; LastFailureTime = $null
        CurrentDelaySec = $CheckIntervalSec; MaxDelaySec = 120
    }
}

try {
    foreach ($instance in $serverInstances) {
        $startResult = Start-NewInstance -InstanceId $instance.InstanceId -Path $Path -ExtraArguments $ExtraArguments -LogToFile:$LogToFile -LogFile:$logFile
        if ($startResult) {
            $instance.Process = $startResult.Process
            $instance.PortInfo = $startResult.PortInfo
            $instance.Status = 'Running'
        } else { $instance.Status = 'Failed' }
        Start-Sleep -Seconds 2
    }

    Write-Verbose "Initial startup complete. Entering monitoring loop. Press Ctrl+C to exit."
    while ($true) {
        Monitor-Instances -ServerInstances $serverInstances -EnableHealthChecks:$EnableHealthChecks -CheckIntervalSec $CheckIntervalSec -ExtraArguments $ExtraArguments -Path $Path -LogToFile:$LogToFile -LogFile:$logFile
        Start-Sleep -Seconds $CheckIntervalSec
    }
}
finally {
    Cleanup-Instances -ServerInstances $serverInstances -LogToFile:$LogToFile -LogFile:$logFile
}
