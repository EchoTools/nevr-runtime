# Path to the EchoVR executable
$path = "C:\echovr\ready-at-dawn-echo-arena\bin\win10\echovr.exe"

# Number of instances to start
$instancesToStart = 4

# Timestep
$timeStep = 180

#Please use one of the following region codes after in $region
  #  "uscn", // US Central North (Chicago)
  #  "us-central-2", // US Central South (Texas)
  #  "us-central-3", // US Central South (Texas)
  #  "use", // US East (Virgina)
  #  "usw", // US West (California)
  #  "euw", // EU West 
  #  "jp", // Japan (idk)
  #  "sin", // Singapore oce region

$region = "echovrce-chi-comp"

# How Often to check for server closed
$check = 5 # Seconds

# Do you want logs? $true or $false
$logToFile = $true 

# Starting ports
$port = 6793 # Default 6792
$httpPort = 6719 #Default 6719

# Define the log folder and log file path
$logFolder = ".\EchoServerLogs" #(.\EchoServerLogs - Will put the log folder in teh same place as the script.)
$logDate = Get-Date -Format "MM-dd-yy"
$logFile = "$logFolder\$logDate.log"

#---------------------------------------------
# No configs below here...

# Initialize an empty list to hold the argument strings
$argumentsList = @()

# Loop to generate arguments for each instance
for ($i = 0; $i -lt $instancesToStart; $i++) {
    $currentPort = $port + $i
    $currentHttpPort = $httpPort + $i
    $arguments = "-noovr -server -headless -fixedtimestep -timestep $timeStep -region $region -exitonerror -port $currentPort -httpport $currentHttpPort"
    $argumentsList += $arguments
}

# Display Config
Write-Host " Running Config:`n   EchovR Path: $path`n   Logs Path: $logFolder`n   Servers to start: $instancesToStart`n   How often to check servers: $check Seconds"

# Check if the folder exists; create it if it doesn't
if ($logToFile) {
    Write-Host "`n Checking for Log Folder..."
    if (-not (Test-Path -Path $logFolder)) {
        New-Item -ItemType Directory -Path $logFolder | Out-Null
        Write-Host "  Log folder created: $logFolder" -ForegroundColor Green
        Start-Sleep -Seconds 1
        }else{
        Write-Host "  Log Path Verified."
        }
}else{
Write-Host "   Logging is disabled in setup."
}

# Set Timestamp
function Get-Timestamp {
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    return $timestamp
}

# Log Function
function LogThis {
    param (
        [string]$logEntry
    )
    Add-Content -Path $logFile -Value $logEntry
}

# Log Start-up and Config
    if ($logToFile) {
    $timestamp = Get-Timestamp
    $logEntry = "$timestamp - New Script Startup, $instancesToStart instances, $check second checks."
    LogThis -logEntry $logEntry
    }

# Function to get currently running instances of the executable
function Get-RunningInstances {
    param (
        [string]$path
    )
    Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -eq $path
    }
}

# Function to start a new instance with specific arguments
function Start-NewInstance {
    param (
        [string]$path,
        [string]$arguments
    )
    $process = Start-Process -FilePath $path -ArgumentList $arguments -PassThru -WindowStyle Minimized
    Start-Sleep -Seconds 1
    $process.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::High
    Start-Sleep -Seconds 1

    # Log New Instance
    if ($logToFile) {
    $timestamp = Get-Timestamp
    $logEntry = "$timestamp - Instance $($i + 1) - PID $($process.Id), $arguments"
    LogThis -logEntry $logEntry
    }

    return $process
}

# Output the generated arguments
Write-Host "`n  Starting Servers with arguments..."
$argumentsList

# Start the specified number of instances
Write-Host "`n  Identifying existing instances..."
$runningProcesses = Get-RunningInstances -path $path

for ($i = 0; $i -lt $instancesToStart; $i++) {
    # Check if the specific instance is already running
    $argumentRegex = [regex]::Escape($argumentsList[$i])
    $matchingProcess = $runningProcesses | Where-Object {
        $_.CommandLine -match $argumentRegex
    }

    if ($matchingProcess) {
        $echoid = $matchingProcess.ProcessId
        Write-Host "    Instance $($i + 1) is already running (PID: $echoid)." -ForegroundColor Cyan

        #Log to File
        if ($logToFile) {
        $timestamp = Get-Timestamp
        $logEntry = "$timestamp - Already Running - Instance $($i + 1) - PID: $echoid"
        LogThis -logEntry $logEntry
        }

    } else {
        Start-NewInstance -path $path -arguments $argumentsList[$i]
    }
}

while ($true) {
    $runningProcesses = Get-RunningInstances -path $path

    for ($i = 0; $i -lt $instancesToStart; $i++) {
        # Check if the specific instance is running
        $argumentRegex = [regex]::Escape($argumentsList[$i])
        $matchingProcess = $runningProcesses | Where-Object {
            $_.CommandLine -match $argumentRegex
        }

        if (-not $matchingProcess) {
            # Log Exit
            if ($logToFile) {
            $timestamp = Get-Timestamp
            $logEntry = "$timestamp - Instance $($i +1) exited!"
            LogThis -logEntry $logEntry
            }
            
            Write-Host "    $timestamp - Instance $($i + 1) has exited. Restarting..." -ForegroundColor Red
            # Restart instance
            Start-NewInstance -path $path -arguments $argumentsList[$i]
        }
    }

    Start-Sleep -Seconds $check
}
