function Test-PortAvailable {
    param([int]$Port)
    try {
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
        $listener.Start(); $listener.Stop()
        return $true
    } catch { return $false }
}

function Get-AvailableTcpPort {
    param([int]$StartPort = 6721)
    $port = $StartPort
    $tempPath = [System.IO.Path]::GetTempPath()
    for ($i = 0; $i -lt 1000; $i++) {
        $portFile = Join-Path $tempPath "cevr_ps1.port.$port"
        if (Test-PortAvailable -Port $port) {
            if (-not (Test-Path $portFile)) {
                $PID | Set-Content -Path $portFile
                return @{ Port = $port; PortFile = $portFile }
            } else {
                $filePid = Get-Content $portFile
                if (-not (Get-Process -Id $filePid -ErrorAction SilentlyContinue)) {
                    Write-Verbose "Removing stale port file: $portFile"
                    Remove-Item $portFile -Force
                    $i--
                }
            }
        }
        $port++
    }
    throw "Could not find an available TCP port after 1000 attempts."
}
