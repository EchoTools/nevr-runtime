# Install a Quest APK and its game data with SideQuest's adb.
# Right-click this file and choose "Run with PowerShell", or run:
# powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\quest-install.ps1 -Apk .\game.apk
# -Yes permits uninstalling an app with an incompatible signature (and deleting its app data).
# The window waits for Enter before it closes so a right-click run can be read; -NoPause skips that.
param(
    [string]$Apk,
    [string]$ApkSha256,
    [string]$Adb,
    [switch]$Yes,
    [switch]$NoPause,
    [string]$DataUrl = 'https://mia.cdn.echo.taxi/_data.zip',
    [string]$DataSha256 = 'fc2eedeacc50d9ddf751e21914bb4188660cf5e79ce48aab24b84e757f4b543c'
)

$ErrorActionPreference = 'Stop'
$package = 'com.readyatdawn.r15'
$remoteZip = '/data/local/tmp/_data.zip'
$needKb = 2306867
$maxBytes = 1100000000

function Quote-Argument([string]$value) {
    if ($value -notmatch '[\s"]') { return $value }
    return '"' + ($value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Invoke-Adb([int]$seconds, [string[]]$arguments) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Adb
    $start.Arguments = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.CreateNoWindow = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw "could not start adb $($arguments -join ' ')" }
        $process.StandardInput.Close()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($seconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "adb $($arguments -join ' ') timed out after $seconds s"
        }
        $output = (($stdout.Result, $stderr.Result) -join '').Trim()
        return [pscustomobject]@{ Code = $process.ExitCode; Output = $output }
    } finally {
        $process.Dispose()
    }
}

function Invoke-CheckedAdb([int]$seconds, [string[]]$arguments) {
    $result = Invoke-Adb $seconds $arguments
    if ($result.Code -ne 0 -or $result.Output -match '(?m)^Failure \[') {
        throw "adb $($arguments -join ' ') failed (exit $($result.Code)): $($result.Output)"
    }
    return $result.Output
}

function Find-Adb {
    if ($Adb) {
        if (-not (Test-Path -LiteralPath $Adb -PathType Leaf)) { throw "adb not found: $Adb" }
        return (Resolve-Path -LiteralPath $Adb).ProviderPath
    }
    $looked = New-Object System.Collections.Generic.List[string]
    $found = New-Object System.Collections.Generic.List[string]
    foreach ($root in @($env:LOCALAPPDATA, $env:APPDATA)) {
        if (-not $root) { $looked.Add('(unset LOCALAPPDATA or APPDATA)'); continue }
        $looked.Add("$root (SideQuest folders, then platform-tools\adb.exe)")
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        # SilentlyContinue: one folder this user cannot read must not stop the search of the others.
        $folders = @(Get-ChildItem -LiteralPath $root -Directory -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'sidequest' })
        if ((Split-Path -Path $root -Leaf) -match 'sidequest') {
            $folders += Get-Item -LiteralPath $root
        }
        foreach ($folder in $folders) {
            $looked.Add("$($folder.FullName)\**\platform-tools\adb.exe")
            Get-ChildItem -LiteralPath $folder.FullName -File -Filter adb.exe -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.Directory.Name -ieq 'platform-tools' } |
                ForEach-Object { if (-not $found.Contains($_.FullName)) { $found.Add($_.FullName) } }
        }
    }
    if ($found.Count -eq 0) {
        throw "SideQuest adb.exe not found. Looked in:`n$($looked -join "`n")`nInstall SideQuest or pass -Adb <path>."
    }
    if ($found.Count -ne 1) { throw "Multiple SideQuest adb.exe files found: $($found -join ', '). Pass -Adb <path>." }
    return $found[0]
}

function Assert-Hash([string]$path, [string]$expected) {
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw "SHA-256 mismatch for $path (expected $expected, got $actual)" }
}

function Get-ApkHash([string]$path) {
    $expected = $ApkSha256
    $sums = Join-Path (Split-Path -Parent $path) 'SHA256SUMS'
    if (Test-Path -LiteralPath $sums -PathType Leaf) {
        $name = Split-Path -Leaf $path
        $entries = @(Get-Content -LiteralPath $sums | Where-Object {
            $_ -match '^([0-9a-fA-F]{64})  \*?(.+)$' -and $Matches[2] -ceq $name
        } | ForEach-Object { ($_ -split '  ', 2)[0].ToLowerInvariant() })
        if ($entries.Count -ne 1) { throw "SHA256SUMS must contain exactly one entry for $name ($sums)" }
        if ($expected -and $expected.ToLowerInvariant() -ne $entries[0]) {
            throw "-ApkSha256 disagrees with $sums for $name"
        }
        $expected = $entries[0]
    }
    if (-not $expected) { $expected = Read-Host 'Enter the APK SHA-256 (no SHA256SUMS entry found)' }
    if ($expected -cnotmatch '^[0-9a-fA-F]{64}$') { throw 'APK SHA-256 must be 64 hex characters' }
    return $expected.ToLowerInvariant()
}

function Get-GameData([string]$destination) {
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -eq $DataSha256) { return }
        Write-Host "Cached $destination has the wrong SHA-256; deleting it."
        Remove-Item -LiteralPath $destination -Force
    }
    $part = "$destination.part"
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $response = $null
        $inputStream = $null
        $outputStream = $null
        try {
            if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
            Write-Host "Downloading game data (~937 MB) to $part (attempt $attempt/3) ..."
            $uri = [uri]$DataUrl
            for ($redirect = 0; $redirect -le 10; $redirect++) {
                if ($uri.Scheme -ne 'https') { throw "download redirected outside HTTPS: $uri" }
                $request = [System.Net.HttpWebRequest]::Create($uri)
                $request.AllowAutoRedirect = $false
                $request.Timeout = 20000
                $request.ReadWriteTimeout = 60000
                $response = $request.GetResponse()
                if ([int]$response.StatusCode -lt 300 -or [int]$response.StatusCode -ge 400) { break }
                if (-not $response.Headers['Location']) { throw "redirect without Location: $uri" }
                $uri = New-Object System.Uri($uri, $response.Headers['Location'])
                $response.Dispose(); $response = $null
            }
            if (-not $response) { throw 'download exceeded 10 redirects' }
            if ($response.ContentLength -gt $maxBytes) { throw "download exceeds $maxBytes bytes" }
            $inputStream = $response.GetResponseStream()
            $outputStream = [System.IO.File]::Create($part)
            $buffer = New-Object byte[] 65536
            $total = 0L
            $clock = [System.Diagnostics.Stopwatch]::StartNew()
            $speedWindow = [System.Diagnostics.Stopwatch]::StartNew()
            $windowBytes = 0L
            while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $total += $count
                $windowBytes += $count
                if ($total -gt $maxBytes) { throw "download exceeds $maxBytes bytes" }
                if ($clock.Elapsed.TotalSeconds -gt 7200) { throw 'download timed out after 7200 s' }
                if ($speedWindow.Elapsed.TotalSeconds -ge 60) {
                    if ($windowBytes / $speedWindow.Elapsed.TotalSeconds -lt 10000) { throw 'download stayed below 10000 bytes/s for 60 s' }
                    $speedWindow.Restart()
                    $windowBytes = 0
                }
                $outputStream.Write($buffer, 0, $count)
            }
            $outputStream.Dispose(); $outputStream = $null
            $actual = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $DataSha256) { throw "downloaded file does not match pinned SHA-256 $DataSha256 (got $actual)" }
            Move-Item -LiteralPath $part -Destination $destination
            return
        } catch {
            $failure = $_.Exception.Message
            if ($failure -like 'downloaded file does not match pinned SHA-256*') { throw $failure }
            if ($attempt -eq 3) { throw "download failed: $failure" }
            Start-Sleep -Seconds 1
        } finally {
            if ($outputStream) { $outputStream.Dispose() }
            if ($inputStream) { $inputStream.Dispose() }
            if ($response) { $response.Dispose() }
            if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
        }
    }
}

try {
    if ($DataSha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'DataSha256 must be 64 lowercase hex characters' }
    if ($DataUrl -notmatch '^https://') { throw 'DataUrl must use HTTPS' }
    [System.Net.ServicePointManager]::SecurityProtocol =
        [System.Net.ServicePointManager]::SecurityProtocol -bor [System.Net.SecurityProtocolType]::Tls12
    if (-not $Apk) {
        $nearby = @(Get-ChildItem -LiteralPath $PSScriptRoot -File -Filter '*.apk')
        if ($nearby.Count -eq 1) { $Apk = $nearby[0].FullName }
        else { $Apk = Read-Host 'Enter the path to the Quest APK' }
    }
    if (-not (Test-Path -LiteralPath $Apk -PathType Leaf)) { throw "APK not found: $Apk" }
    $Apk = (Resolve-Path -LiteralPath $Apk).ProviderPath
    $expectedApkHash = Get-ApkHash $Apk
    Assert-Hash $Apk $expectedApkHash
    Write-Host "APK verified (sha256 $expectedApkHash)"
    $Adb = Find-Adb
    Write-Host "Using adb: $Adb"

    $devices = Invoke-CheckedAdb 30 @('devices')
    $rows = @($devices -split "`r?`n" | Where-Object {
        $_ -match '^\S+\s+(device|offline|unauthorized|authorizing|connecting|recovery|sideload|rescue|bootloader|host|unknown|detached|no)(\s|$)'
    })
    if ($rows.Count -eq 0) { throw 'No adb device attached. Connect the headset by USB, enable USB debugging, and retry.' }
    if ($rows.Count -ne 1) { throw "$($rows.Count) adb devices attached; disconnect extras and retry: $($rows -join ', ')" }
    $fields = $rows[0] -split '\s+', 2
    $serial = $fields[0]
    $state = $fields[1].Trim()
    if ($state -like 'no permissions*') { throw "Device ${serial}: $state (usually missing udev rules for the headset, or your user is not in the plugdev group)" }
    if ($state -eq 'unauthorized') { throw "Device $serial is unauthorized. Accept the USB debugging prompt in the headset and retry." }
    if ($state -ne 'device') { throw "Device $serial is in state '$state'; reconnect it and retry." }
    Invoke-CheckedAdb 30 @('-s', $serial, 'wait-for-device') | Out-Null

    # `pm path` exits 1 with no output when the package is absent: that is a first install, not a failure.
    $pm = Invoke-Adb 30 @('-s', $serial, 'shell', 'pm', 'path', $package)
    if ($pm.Code -eq 0) { Write-Host "$package is installed on $serial" }
    elseif (-not $pm.Output) { Write-Host "$package is not installed on $serial" }
    else { throw "package check failed (exit $($pm.Code)): $($pm.Output)" }
    foreach ($path in @('/sdcard', '/data/local/tmp')) {
        $df = Invoke-CheckedAdb 30 @('-s', $serial, 'shell', "df -Pk $path")
        $lines = @($df -split "`r?`n")
        if ($lines.Count -lt 2) { throw "could not read free space on $path`: $df" }
        $columns = $lines[1].Trim() -split '\s+'
        if ($columns.Count -lt 4 -or $columns[3] -notmatch '^\d+$') { throw "could not read free space on $path`: $df" }
        if ([long]$columns[3] -lt $needKb) { throw "$path has $([math]::Floor([long]$columns[3] / 1024)) MiB free, need $([math]::Floor($needKb / 1024)) MiB" }
    }

    $cacheRoot = Join-Path $env:LOCALAPPDATA 'nevr\quest-data'
    New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null
    $dataName = if ($DataSha256 -eq 'fc2eedeacc50d9ddf751e21914bb4188660cf5e79ce48aab24b84e757f4b543c') { '_data.zip' } else { "_data-$DataSha256.zip" }
    $data = Join-Path $cacheRoot $dataName
    Get-GameData $data
    Write-Host "Game data verified (sha256 $DataSha256)"

    Write-Host "Installing $Apk ..."
    $install = Invoke-Adb 600 @('-s', $serial, 'install', '-r', '-g', $Apk)
    if ($install.Output) { Write-Host $install.Output }
    if ($install.Code -ne 0 -or $install.Output -match 'Failure \[') {
        if ($install.Output -notmatch 'INSTALL_FAILED_UPDATE_INCOMPATIBLE') {
            throw "adb install failed (exit $($install.Code)): $($install.Output)"
        }
        if (-not $Yes) {
            throw "The installed $package is signed differently. Uninstalling deletes its app data on the headset (/data/data/$package, /sdcard/Android/data/$package, and its OBB). Re-run with -Yes to allow it."
        }
        Write-Host "Uninstalling $package (deletes its app data) ..."
        Invoke-CheckedAdb 120 @('-s', $serial, 'uninstall', $package) | Out-Null
        Invoke-CheckedAdb 600 @('-s', $serial, 'install', '-g', $Apk) | Out-Null
    }

    Write-Host 'Pushing + extracting game data to /sdcard/readyatdawn ...'
    $pushError = $null
    try {
        Invoke-CheckedAdb 1800 @('-s', $serial, 'push', $data, $remoteZip) | Out-Null
        Invoke-CheckedAdb 900 @('-s', $serial, 'shell', "mkdir -p /sdcard/readyatdawn && cd /sdcard/readyatdawn && unzip -o $remoteZip") | Out-Null
    } catch { $pushError = $_.Exception.Message }
    $cleanupError = $null
    try {
        $cleanup = Invoke-Adb 60 @('-s', $serial, 'shell', "rm -f $remoteZip")
        if ($cleanup.Code -ne 0) { $cleanupError = "remote cleanup failed (exit $($cleanup.Code)): $($cleanup.Output)" }
    } catch { $cleanupError = "remote cleanup failed: $($_.Exception.Message)" }
    if ($cleanupError) { throw "${pushError} $cleanupError".Trim() }
    if ($pushError) { throw $pushError }
    Write-Host 'Done.'
} catch {
    [Console]::Error.WriteLine("error: $($_.Exception.Message)")
    $failed = $true
}
if (-not $NoPause) { Read-Host 'Press Enter to close' | Out-Null }
if ($failed) { exit 1 }
