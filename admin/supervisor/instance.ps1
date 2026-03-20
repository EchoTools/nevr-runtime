function Start-NewInstance {
    param(
        [int]$InstanceId,
        [string]$Path,
        [string]$ExtraArguments,
        [boolean]$LogToFile,
        [string]$LogFile
    )
    try {
        $portInfo = Get-AvailableTcpPort
        $baseArgs = "-noovr -server -headless -fixedtimestep -exitonerror"
        $arguments = "$baseArgs -httpport $($portInfo.Port) $ExtraArguments"

        Write-Verbose "Starting Instance #$InstanceId with args: $arguments"
        $process = Start-Process -FilePath $Path -ArgumentList $arguments -PassThru -WindowStyle Minimized -ErrorAction Stop
        Start-Sleep -Milliseconds 250
        $process.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::High

        Write-LogEntry -Message "Started Instance #$InstanceId (PID $($process.Id)) with args: $arguments" -LogToFile:$LogToFile -LogFile:$LogFile
        return @{ Process = $process; PortInfo = $portInfo }
    } catch {
        Write-Error "Failed to start instance #$InstanceId. Error: $_"
        Write-LogEntry -Message "ERROR: Failed to start instance #$InstanceId. Details: $_" -LogToFile:$LogToFile -LogFile:$LogFile
        if ($portInfo.PortFile) { Remove-Item $portInfo.PortFile -Force }
        return $null
    }
}

function Monitor-Instances {
    param(
        [array]$ServerInstances,
        [switch]$EnableHealthChecks,
        [int]$CheckIntervalSec,
        [string]$ExtraArguments,
        [string]$Path,
        [boolean]$LogToFile,
        [string]$LogFile
    )
    foreach ($instance in $ServerInstances) {
        $isProcessAlive = $instance.Process -ne $null -and -not $instance.Process.HasExited

        if ($isProcessAlive -and $EnableHealthChecks) {
            try {
                $uri = "http://127.0.0.1:$($instance.PortInfo.Port)"
                Invoke-WebRequest -Uri $uri -TimeoutSec 2 -ErrorAction Stop | Out-Null
            } catch {
                Write-Warning "Health check FAILED for Instance #$($instance.InstanceId) (PID: $($instance.Process.Id)). Terminating."
                Write-LogEntry -Message "Health check failed for PID $($instance.Process.Id). Terminating." -LogToFile:$LogToFile -LogFile:$LogFile
                Stop-Process -Id $instance.Process.Id -Force
                $isProcessAlive = $false
            }
        }

        if (-not $isProcessAlive -and $instance.Status -eq 'Running') {
            Write-Warning "Instance #$($instance.InstanceId) (PID: $($instance.Process.Id)) has exited."
            Write-LogEntry -Message "Instance #$($instance.InstanceId) (PID: $($instance.Process.Id)) exited." -LogToFile:$LogToFile -LogFile:$LogFile

            if (((Get-Date) - $instance.Process.StartTime).TotalSeconds -lt 30) {
                Write-Warning "Fast crash detected. Applying backoff delay of $($instance.CurrentDelaySec) seconds."
                $instance.CurrentDelaySec = [System.Math]::Min($instance.CurrentDelaySec * 2, $instance.MaxDelaySec)
            } else { $instance.CurrentDelaySec = $CheckIntervalSec }

            $instance.Status = 'Stopped'; $instance.LastFailureTime = Get-Date
        }

        if ($instance.Status -ne 'Running') {
            $timeSinceFailure = if ($instance.LastFailureTime) { (Get-Date) - $instance.LastFailureTime } else { [System.TimeSpan]::MaxValue }
            if ($timeSinceFailure.TotalSeconds -ge $instance.CurrentDelaySec) {
                if ($instance.PortInfo.PortFile) { Remove-Item $instance.PortInfo.PortFile -Force -ErrorAction SilentlyContinue }

                $startResult = Start-NewInstance -InstanceId $instance.InstanceId -Path $Path -ExtraArguments $ExtraArguments -LogToFile:$LogToFile -LogFile:$LogFile
                if ($startResult) {
                    $instance.Process = $startResult.Process; $instance.PortInfo = $startResult.PortInfo
                    $instance.Status = 'Running'
                } else { $instance.Status = 'Failed'; $instance.LastFailureTime = Get-Date }
            }
        }
    }
}

function Cleanup-Instances {
    param(
        [array]$ServerInstances,
        [boolean]$LogToFile,
        [string]$LogFile
    )
    Write-Warning "Script terminating. Shutting down all server instances..."
    foreach ($instance in $ServerInstances) {
        if ($instance.Process -ne $null -and -not $instance.Process.HasExited) {
            Write-Verbose "Stopping process PID $($instance.Process.Id)..."
            $instance.Process.Kill()
        }
        if ($instance.PortInfo.PortFile -and (Test-Path $instance.PortInfo.PortFile)) {
            Write-Verbose "Removing port file $($instance.PortInfo.PortFile)..."
            Remove-Item $instance.PortInfo.PortFile -Force
        }
    }
    Write-LogEntry -Message "Script terminated by user. All instances stopped." -LogToFile:$LogToFile -LogFile:$LogFile
    Write-Warning "Cleanup complete."
}
