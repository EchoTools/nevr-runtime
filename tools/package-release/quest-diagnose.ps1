# NEVR runtime release: collect what we need to see when the Quest build will not start.
#
# Reads facts from ONE headset over adb (headset model and OS build, the game's version, the data folders,
# the runtime's own log and crash files), then starts the game ONCE and reads the log of that launch. It
# prints a one-screen verdict first, then writes everything to nevr-diagnose-<UTC>.zip next to this script.
# Send us that zip. It never installs or uninstalls anything and deletes nothing on the headset; on the PC it
# deletes only its own temporary folder. On the headset it does three things: it closes the game if it is running,
# it erases the headset's log buffer (after saving a copy as logcat_before_launch.txt), and it starts the game once.
#
# Not collected: device_login.txt (the sign-in link and code), the credential cache, config files. Only the
# files named above are copied. The adb serial, the hardware serial (ro.serialno) and any e-mail address are
# replaced with <device-serial> and <email> in every text file and in the verdict. NOT masked: the zip holds
# the runtime's crash dumps as they are and the headset's whole system log, which can show the Wi-Fi name, the
# BSSID, IP addresses and numeric user ids. Do not attach it to a public GitHub issue; send it to a
# maintainer by direct message on the EchoVRCE Discord, never in a public channel.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File quest-diagnose.ps1 [-Adb <path to adb>] [-Pause | -NoPause]
# adb is searched on the PATH, then in SideQuest's folder (%APPDATA%\SideQuest\platform-tools\adb.exe).
# Connect exactly one headset by USB with USB debugging on and the game installed.
param(
  [string]$Adb = '',
  [string]$OutDir = '',
  [int]$LaunchWaitSeconds = 20,
  [switch]$Pause,
  [switch]$NoPause
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$package = 'com.readyatdawn.r15'
$activity = 'com.oculus.gles3jni.MainActivity'
$mediaRoot = "/sdcard/Android/media/$package"
$legacyRoot = '/sdcard/readyatdawn'
$filesDir = "/sdcard/Android/data/$package/files"

# Same behaviour as install.ps1 (#736): every error is printed plainly and appended to a log next to this script,
# and the window waits for Enter when it was opened from Explorer, or with -Pause; -NoPause never waits.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptName = Split-Path -Leaf $MyInvocation.MyCommand.Path
$logFile = Join-Path $here 'nevr-diagnose.log'
function Write-RunLog([string]$Text) {
  try {
    Add-Content -LiteralPath $logFile -Encoding UTF8 -Value ((Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + ' ' + $Text)
  } catch {
    Write-Host ('could not write the log ' + $logFile + ': ' + $_.Exception.Message)
  }
}
function Test-LaunchedFromExplorer {
  try {
    $parentId = (Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$PID").ParentProcessId
    return ((Get-Process -Id $parentId).ProcessName -eq 'explorer')
  } catch {
    return $false
  }
}
function Wait-BeforeClose {
  if ($NoPause) { return }
  if (-not $Pause) {
    if (-not (Test-LaunchedFromExplorer)) { return }
  }
  try {
    Read-Host 'Press Enter to close' | Out-Null
  } catch {
    Write-Host ('could not wait for Enter: ' + $_.Exception.Message)
  }
}

function Quote-Argument([string]$value) {
  if ($value -notmatch '[\s"]') { return $value }
  return '"' + ($value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

# One adb call with a time limit; the result carries the exit code and stdout+stderr together.
function Invoke-Adb([int]$seconds, [string[]]$arguments) {
  $start = New-Object System.Diagnostics.ProcessStartInfo
  $start.FileName = $script:AdbPath
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
    return [pscustomobject]@{ Code = $process.ExitCode; Output = (($stdout.Result, $stderr.Result) -join '').Trim() }
  } finally {
    $process.Dispose()
  }
}

# adb against the chosen headset. The serial is never written anywhere.
function Invoke-Device([int]$seconds, [string[]]$arguments) {
  return (Invoke-Adb $seconds (@('-s', $script:Serial) + $arguments))
}

function Find-Adb {
  if ($Adb) {
    if (-not (Test-Path -LiteralPath $Adb -PathType Leaf)) { throw "adb not found: $Adb" }
    return (Resolve-Path -LiteralPath $Adb).ProviderPath
  }
  $onPath = Get-Command adb -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($onPath) { return $onPath.Source }
  $sideQuest = ''
  if ($env:APPDATA) {
    $sideQuest = Join-Path (Join-Path (Join-Path $env:APPDATA 'SideQuest') 'platform-tools') 'adb.exe'
    if (Test-Path -LiteralPath $sideQuest -PathType Leaf) { return $sideQuest }
  }
  $where = if ($sideQuest) { $sideQuest } else { '(APPDATA is not set)' }
  throw "adb was not found on the PATH or in SideQuest's folder ($where). Install SideQuest, or run this script with -Adb <path to adb>."
}

# The one connected headset: exactly one row, in the state `device`.
function Get-OneSerial {
  $devices = Invoke-Adb 30 @('devices')
  if ($devices.Code -ne 0) { throw "adb devices failed (exit $($devices.Code)): $($devices.Output)" }
  $rows = @($devices.Output -split "`r?`n" | Where-Object {
      $_ -match '^\S+\s+(device|offline|unauthorized|authorizing|connecting|recovery|sideload|rescue|bootloader|host|unknown|detached|no)(\s|$)'
    })
  if ($rows.Count -eq 0) { throw 'No headset is connected. Connect it by USB, turn on USB debugging, accept the prompt in the headset, and run this again.' }
  if ($rows.Count -ne 1) { throw "$($rows.Count) devices are connected; disconnect all but the headset and run this again." }
  $fields = $rows[0] -split '\s+', 2
  $state = $fields[1].Trim()
  if ($state -like 'no permissions*') { throw "The headset reports: $state (usually missing USB rules for the headset on this PC)." }
  if ($state -eq 'unauthorized') { throw 'The headset is unauthorized. Put it on, accept the USB debugging prompt, and run this again.' }
  if ($state -ne 'device') { throw "The headset is in state '$state'; reconnect it and run this again." }
  return $fields[0]
}

# The hardware serial also shows up in logcat when the headset is connected over Wi-Fi (the adb serial is then an address).
function Get-HardwareSerial {
  $result = Invoke-Device 30 @('shell', 'getprop', 'ro.serialno')
  $value = $result.Output.Trim()
  if ($result.Code -eq 0 -and $value -match '^[A-Za-z0-9._:-]{4,}$') { return $value }
  return ''
}

# Replaces both serials and anything shaped like an e-mail address.
function Protect-Text([string]$text) {
  if ($null -eq $text) { return '' }
  if ($script:Serial) { $text = $text.Replace($script:Serial, '<device-serial>') }
  if ($script:HwSerial) { $text = $text.Replace($script:HwSerial, '<device-serial>') }
  return [regex]::Replace($text, '[A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(\.[A-Za-z0-9\-]+)+', '<email>')
}

function Save-Text([string]$relative, [string]$text) {
  $path = Join-Path $script:Work $relative
  $folder = Split-Path -Parent $path
  if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
  [System.IO.File]::WriteAllText($path, (Protect-Text $text), (New-Object System.Text.UTF8Encoding($false)))
}

function Get-DeviceText([int]$seconds, [string[]]$arguments) {
  $result = Invoke-Device $seconds $arguments
  return $result.Output
}

$exitCode = 0
$script:AdbPath = ''
$script:Serial = ''
$script:HwSerial = ''
$script:Work = ''
$zipPath = ''
try {
  $script:AdbPath = Find-Adb
  Write-Host "Using adb: $script:AdbPath"
  $script:Serial = Get-OneSerial
  $script:HwSerial = Get-HardwareSerial
  $utc = [DateTime]::UtcNow
  $stamp = $utc.ToString('yyyyMMdd\THHmmss\Z')
  if (-not $OutDir) { $OutDir = $here }
  if (-not (Test-Path -LiteralPath $OutDir -PathType Container)) { throw "the output folder does not exist: $OutDir" }
  $zipPath = Join-Path $OutDir "nevr-diagnose-$stamp.zip"
  if (Test-Path -LiteralPath $zipPath) { throw "$zipPath exists; not overwritten" }
  $script:Work = Join-Path ([System.IO.Path]::GetTempPath()) "nevr-diagnose-$stamp"
  New-Item -ItemType Directory -Path $script:Work -Force | Out-Null

  Write-Host 'Reading the headset ...'
  $model = Get-DeviceText 30 @('shell', 'getprop', 'ro.product.model')
  $osBuild = Get-DeviceText 30 @('shell', 'getprop', 'ro.build.display.id')
  $release = Get-DeviceText 30 @('shell', 'getprop', 'ro.build.version.release')
  Save-Text 'device.txt' ("model: $model`nos build: $osBuild`nandroid release: $release`n")

  $packageDump = Get-DeviceText 60 @('shell', 'dumpsys', 'package', $package)
  $versionCode = if ($packageDump -match 'versionCode=(\d+)') { $Matches[1] } else { '' }
  $versionName = if ($packageDump -match 'versionName=(\S+)') { $Matches[1] } else { '' }
  Save-Text 'package.txt' ("package: $package`nversionCode: $versionCode`nversionName: $versionName`n")

  $installerRaw = Get-DeviceText 30 @('shell', "cat $mediaRoot/.echo_installer_version")
  $installer = if ($installerRaw -and $installerRaw -notmatch 'No such file|not found') { $installerRaw.Trim() } else { '' }
  Save-Text 'installer_version.txt' $(if ($installer) { "$installer`n" } else { "absent`n" })

  # Listings: names and sizes, one level, plus the one level under _data. Never file contents.
  $listings = [ordered]@{
    'readyatdawn.txt'            = $legacyRoot
    'readyatdawn_data.txt'       = "$legacyRoot/_data"
    'media.txt'                  = $mediaRoot
    'media_files.txt'            = "$mediaRoot/files"
    'media_files_data.txt'       = "$mediaRoot/files/_data"
    'media_asset_patches.txt'    = "$mediaRoot/asset_patches"
    'android_data_files.txt'     = $filesDir
  }
  foreach ($name in $listings.Keys) {
    $dir = $listings[$name]
    Save-Text "listings/$name" ("ls -la $dir`n" + (Get-DeviceText 60 @('shell', "ls -la $dir")) + "`n")
  }
  function Test-RemoteDir([string]$dir) {
    return ((Get-DeviceText 30 @('shell', "[ -d $dir ] && echo yes || echo no")).Trim() -eq 'yes')
  }
  $dataMedia = Test-RemoteDir "$mediaRoot/files/_data"
  $dataLegacy = Test-RemoteDir "$legacyRoot/_data"
  # The runtime also serves the Android/media files folder when it exists without _data (android_media_unmarked, #749).
  $mediaDir = Test-RemoteDir "$mediaRoot/files"

  # The runtime's own log and crash files: nevr-sentinel*.log (this session, the kept previous sessions, the size-rotated
  # files) and nevr-crashes. The Android/media files folder is read first (where the runtime writes them, and where a
  # player can reach them); the Android/data folder is the fallback.
  New-Item -ItemType Directory -Path (Join-Path $script:Work 'sentinel') -Force | Out-Null
  $logPath = Join-Path $script:Work 'sentinel/nevr-sentinel.log'
  $logDir = ''
  $sentinelLog = $null
  foreach ($candidate in @("$mediaRoot/files", $filesDir)) {
    $sentinelLog = Invoke-Device 120 @('pull', "$candidate/nevr-sentinel.log", $logPath)
    if ($sentinelLog.Code -eq 0) { $logDir = $candidate; break }
  }
  $keptPulls = ''
  if ($logDir) {
    $listing = Get-DeviceText 30 @('shell', "ls $logDir")
    foreach ($name in @($listing -split "`r?`n" | Where-Object { $_ -match '^nevr-sentinel\..+\.log$' })) {
      $kept = Invoke-Device 120 @('pull', "$logDir/$name", (Join-Path $script:Work "sentinel/$name"))
      $keptPulls += "${name}: exit $($kept.Code) $($kept.Output)`n"
    }
  }
  $crashes = $null
  foreach ($candidate in @("$mediaRoot/files", $filesDir)) {
    $crashes = Invoke-Device 120 @('pull', "$candidate/nevr-crashes", (Join-Path $script:Work 'sentinel'))
    if ($crashes.Code -eq 0) { break }
  }
  $logFiles = @(Get-ChildItem -LiteralPath (Join-Path $script:Work 'sentinel') -File -Filter 'nevr-sentinel*.log' -ErrorAction SilentlyContinue)
  foreach ($file in $logFiles) { Save-Text ($file.FullName.Substring($script:Work.Length + 1)) ([System.IO.File]::ReadAllText($file.FullName)) }
  $logFullNames = @($logFiles | ForEach-Object { $_.FullName })
  $crashFiles = @(Get-ChildItem -LiteralPath (Join-Path $script:Work 'sentinel') -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object { $logFullNames -notcontains $_.FullName })
  foreach ($file in $crashFiles) {
    if ($file.Extension -in '.log', '.txt', '.json') { Save-Text ($file.FullName.Substring($script:Work.Length + 1)) ([System.IO.File]::ReadAllText($file.FullName)) }
  }
  $folderName = if ($logDir) { $logDir } else { 'none' }
  Save-Text 'sentinel/pull_results.txt' ("nevr-sentinel.log: exit $($sentinelLog.Code) $($sentinelLog.Output) (folder: $folderName)`n${keptPulls}nevr-crashes: exit $($crashes.Code) $($crashes.Output)`n")

  # ONE clean launch. The headset's log of the run the tester came with is saved first: logcat -c erases it.
  Write-Host "Starting the game once and waiting $LaunchWaitSeconds s (this closes it first if it is running and erases the headset's log buffer after saving a copy) ..."
  Save-Text 'logcat_before_launch.txt' (Get-DeviceText 120 @('logcat', '-d', '-b', 'all'))
  Invoke-Device 30 @('shell', 'am', 'force-stop', $package) | Out-Null
  Invoke-Device 30 @('logcat', '-c') | Out-Null
  $start = Invoke-Device 60 @('shell', 'am', 'start', '-n', "$package/$activity")
  if ($LaunchWaitSeconds -gt 0) { Start-Sleep -Seconds $LaunchWaitSeconds }
  $pidResult = Invoke-Device 30 @('shell', 'pidof', $package)
  $running = ($pidResult.Code -eq 0 -and $pidResult.Output -match '\d')
  $logcat = Invoke-Device 120 @('logcat', '-d', '-b', 'all')
  Save-Text 'launch.txt' ("am start: $($start.Output)`nrunning after ${LaunchWaitSeconds} s: $running`n")
  Save-Text 'logcat.txt' $logcat.Output

  # The verdict, from what was just read.
  $log = $logcat.Output
  $obb = if ($log -match "OBB mounted at '([^']*)'") { $Matches[1] } else { '' }
  $dataRoot = ''
  if ($log -match '(?m)^.*"event":"data_root".*$') {
    $line = $Matches[0]
    $dataRoot = if ($line -match '"result":"([^"]*)"') { 'result=' + $Matches[1] } else { 'present' }
    if ($line -match '"source":"([^"]*)"') { $dataRoot += ' source=' + $Matches[1] }
  }
  $crashMarker = if ($log -match 'FATAL EXCEPTION|Fatal signal|am_crash|ANR in ') { $Matches[0] } else { '' }
  $startFailed = ($start.Output -match 'Error')

  $problems = New-Object System.Collections.Generic.List[string]
  if (-not $dataMedia -and -not $dataLegacy -and -not $mediaDir) { $problems.Add("NO GAME DATA: neither $mediaRoot/files nor $legacyRoot/_data exists.") }
  if ($startFailed) { $problems.Add('The game did not start: ' + $start.Output) }
  elseif (-not $running) { $problems.Add("The game was not running $LaunchWaitSeconds s after it started (it ended or crashed).") }
  if ($crashMarker) { $problems.Add("The launch log has a crash marker: $crashMarker") }
  # #764: a failure is an exit, a death, a crash marker or the game's own Initialization Error. A line a build does not
  # write (the OBB mount line) is never a failure of a game that is still running.
  if ($log -match '(?m)^.*(Initialization Error.*)$') {
    $initError = $Matches[1].TrimEnd("`r")
    $initFile = [regex]::Match($log, 'Failed to find package manifest file [^ \r\n]*').Value
    $problems.Add("The game logged: $initError" + $(if ($initFile) { " ($initFile)" } else { '' }))
  }
  $result = if ($problems.Count -eq 0 -and -not $obb -and -not $dataRoot) {
    "No failure seen: running after ${LaunchWaitSeconds} s; this build logged no data-mount line, so the data step can't be confirmed from the log. Send the zip anyway if the game still misbehaves."
  } elseif ($problems.Count -eq 0) { 'No problem found in these facts. Send the zip anyway if the game still misbehaves.' } else { $problems -join "`n           " }

  $yesNo = { param($b) if ($b) { 'present' } else { 'absent' } }
  $mediaNote = if (-not $dataMedia -and $mediaDir) { ' (the files folder exists; the runtime serves it as android_media_unmarked)' } else { '' }
  $verdict = @(
    "NEVR Quest diagnose  $($utc.ToString('yyyy-MM-dd HH:mm:ss'))Z",
    "Headset:   $model, OS build $osBuild",
    "Game:      $package versionCode $(if ($versionCode) { $versionCode } else { '(not installed?)' }), versionName $(if ($versionName) { $versionName } else { '-' })",
    "Installer: $(if ($installer) { $installer } else { 'no .echo_installer_version found' })",
    "Data:      $mediaRoot/files/_data: $(& $yesNo $dataMedia)$mediaNote",
    "           $legacyRoot/_data: $(& $yesNo $dataLegacy)",
    "Launch:    the game mounted data at: $(if ($obb) { $obb } else { '(no line)' })",
    "           runtime data_root line: $(if ($dataRoot) { $dataRoot } else { 'none (this build does not log one)' })",
    "           running after ${LaunchWaitSeconds} s: $(if ($running) { 'yes' } else { 'no' })",
    "Verdict:   $result",
    "Zip:       $(Split-Path -Leaf $zipPath)  (in the -OutDir folder, else next to this script; send this file)"
  ) -join "`n"
  $verdict = Protect-Text $verdict
  Save-Text 'verdict.txt' ($verdict + "`n")
  Write-Host ''
  Write-Host $verdict

  # The zip: verdict first, forward slashes in entry names on every Windows PowerShell.
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $files = @(Get-ChildItem -LiteralPath $script:Work -Recurse -File)
  $ordered = @($files | Where-Object { $_.Name -eq 'verdict.txt' }) + @($files | Where-Object { $_.Name -ne 'verdict.txt' } | Sort-Object FullName)
  $zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
  try {
    foreach ($file in $ordered) {
      $entry = $file.FullName.Substring($script:Work.Length).TrimStart('\', '/').Replace('\', '/')
      [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
  } finally {
    $zip.Dispose()
  }
  Write-RunLog ("OK $scriptName wrote $zipPath problems=$($problems.Count)")
  Write-Host "Privacy: this zip holds crash dumps and the headset's system log (Wi-Fi name, BSSID, IP addresses, numeric user ids). Do not attach it to a public GitHub issue; send it to a maintainer by direct message on the EchoVRCE Discord: it is never in a public channel."
} catch {
  $exitCode = 1
  $message = Protect-Text $_.Exception.Message
  Write-Host ''
  Write-Host ('ERROR: ' + $message)
  Write-Host ('(also written to ' + $logFile + ')')
  Write-RunLog ($scriptName + ' ERROR: ' + $message)
} finally {
  if ($script:Work -and (Test-Path -LiteralPath $script:Work)) { Remove-Item -LiteralPath $script:Work -Recurse -Force -ErrorAction SilentlyContinue }
}
Wait-BeforeClose
exit $exitCode
