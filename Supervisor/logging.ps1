function Initialize-LogFile {
    param(
        [string]$LogFolder
    )
    if (-not (Test-Path -Path $LogFolder)) {
        New-Item -ItemType Directory -Path $LogFolder | Out-Null
    }
    $logDate = Get-Date -Format "MM-dd-yy"
    return (Join-Path -Path $LogFolder -ChildPath "$logDate.log")
}

function Write-LogEntry {
    param(
        [string]$Message,
        [boolean]$LogToFile,
        [string]$LogFile
    )
    if ($LogToFile) {
        $logEntry = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') - $Message"
        try { Add-Content -Path $LogFile -Value $logEntry }
        catch { Write-Warning "Failed to write to log file: $LogFile" }
    }
}
