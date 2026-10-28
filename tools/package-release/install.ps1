# NEVR runtime release: install.
#
# Puts this package's two files in place: BugSplat64.dll (the host the game loads) into the Echo VR install, and
# nevr-runtime.dll (the runtime the host loads) into the player's nevr folder, %LOCALAPPDATA%\nevr\echovr (ADR 0012).
# The install folder keeps only the host, so a runtime update never writes there. It deletes nothing: the original
# BugSplat64.dll is copied to BugSplat64.dll.original-<timestamp>, a legacy dbgcore.dll is renamed to
# dbgcore.dll.legacy-<date>, and a nevr-runtime.dll that an earlier release left beside echovr.exe (the host no longer
# looks there) is renamed to nevr-runtime.dll.legacy-<timestamp>. uninstall.ps1 puts all three back.
# Run again from a newer package, it updates both files in place and leaves those set-aside files alone.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-Dir <...\ready-at-dawn-echo-arena\bin\win10>] [-Root <nevr folder>] [-Pause | -NoPause]
# Without -Dir the usual Meta Horizon / Oculus library locations are searched; with more than one
# install found, pass -Dir. -Root names the nevr folder when %LOCALAPPDATA% is not the player's (an administrator's
# shell, a service account); the record remembers it, so uninstall.ps1 needs no argument.
param([string]$Dir = '', [string]$Root = '', [switch]$Pause, [switch]$NoPause)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }

# #736: a tester who double-clicks this script must see why it stopped. Every error is printed plainly, appended
# with the time and the folder to nevr-install.log next to this script, and the window waits for Enter when it was
# opened from Explorer (it closes the moment the script ends) or when -Pause is given; and -NoPause never waits.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptName = Split-Path -Leaf $MyInvocation.MyCommand.Path
$logFile = Join-Path $here 'nevr-install.log'
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
    # Read-Host throws in a non-interactive host: the script's own result stays its exit code.
    Write-Host ('could not wait for Enter: ' + $_.Exception.Message)
  }
}
$exitCode = 0
try {
$dll = Join-Path $here 'BugSplat64.dll'
$runtimeDll = Join-Path $here 'nevr-runtime.dll'
$sums = Join-Path $here 'SHA256SUMS'
if (-not (Test-Path -LiteralPath $dll)) { throw "BugSplat64.dll is not next to install.ps1 ($here)" }
if (-not (Test-Path -LiteralPath $runtimeDll)) { throw "nevr-runtime.dll is not next to install.ps1 ($here)" }
if (-not (Test-Path -LiteralPath $sums)) { throw "SHA256SUMS is not next to install.ps1 ($here)" }
$wanted = @{}
foreach ($line in Get-Content -LiteralPath $sums) {
  if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') { $wanted[$Matches[2]] = $Matches[1].ToLower() }
}
foreach ($name in 'BugSplat64.dll', 'nevr-runtime.dll') {
  if (-not $wanted.ContainsKey($name)) { throw "SHA256SUMS has no entry for $name" }
}
$ours = Get-Sha $dll
if ($ours -ne $wanted['BugSplat64.dll']) { throw "BugSplat64.dll does not match SHA256SUMS (got $ours): the package is damaged, download it again" }
$oursRuntime = Get-Sha $runtimeDll
if ($oursRuntime -ne $wanted['nevr-runtime.dll']) { throw "nevr-runtime.dll does not match SHA256SUMS (got $oursRuntime): the package is damaged, download it again" }

# Join-Path throws DriveNotFoundException for a drive this PC does not have (#679), so a candidate
# folder is joined by plain concatenation and tested with File.Exists, which answers False instead.
function Get-ExePath([string]$Folder) { [System.IO.Path]::Combine($Folder, 'echovr.exe') }
function Test-Echo([string]$Folder) { [System.IO.File]::Exists((Get-ExePath $Folder)) }

# The library folders the Oculus / Meta Horizon app records for this user (value Path, or
# OriginalPath when the library was moved). A missing key or an unreadable one adds nothing.
function Get-LibraryRoots {
  $out = @()
  try {
    $key = 'HKCU:\Software\Oculus VR, LLC\Oculus\Libraries'
    if (Test-Path -LiteralPath $key) {
      foreach ($lib in Get-ChildItem -LiteralPath $key) {
        foreach ($name in 'Path', 'OriginalPath') {
          $value = $lib.GetValue($name)
          if ($value) { $out += ([string]$value) -replace '^\\\\\?\\', '' }
        }
      }
    }
  } catch {
    Write-Warning ('the Oculus library list could not be read: ' + $_.Exception.Message)
  }
  return $out
}

if ($Dir -eq '') {
  $roots = @('C:\Program Files\Meta Horizon', 'C:\Program Files\Oculus', 'C:\Program Files (x86)\Oculus',
             'D:\Oculus', 'D:\Meta Horizon', 'E:\Oculus', 'E:\Meta Horizon') + @(Get-LibraryRoots)
  $suffixes = @('Software\Software\ready-at-dawn-echo-arena\bin\win10', 'Software\ready-at-dawn-echo-arena\bin\win10',
                'ready-at-dawn-echo-arena\bin\win10')
  $found = @()
  # Not $root: PowerShell variable names are case-insensitive, and this loop would overwrite the -Root parameter.
  foreach ($libraryRoot in $roots) {
    foreach ($suffix in $suffixes) {
      $location = $libraryRoot.TrimEnd('\', '/') + '\' + $suffix
      if ((Test-Echo $location) -and ($found -inotcontains $location)) { $found += $location }
    }
  }
  if ($found.Count -gt 1) { throw ('more than one Echo VR install found; pass -Dir with one of: ' + ($found -join '; ')) }
  if ($found.Count -eq 1) {
    $Dir = $found[0]
  } else {
    $answer = Read-Host 'Echo VR was not found in the usual places. Type the folder that holds echovr.exe (...\ready-at-dawn-echo-arena\bin\win10)'
    if ($null -ne $answer) { $answer = $answer.Trim().Trim('"') }
    if (-not $answer) { throw 'Echo VR was not found in the usual places; pass -Dir <...\bin\win10>' }
    $Dir = $answer
  }
}
if (-not (Test-Echo $Dir)) { throw "no echovr.exe in $Dir" }
if (@(Get-Process -Name echovr -ErrorAction SilentlyContinue).Count -gt 0) { throw 'echovr.exe is running; close the game first' }

# ADR 0012: nevr-runtime.dll lives in the player's nevr folder, not in the game's. Resolved before anything is changed.
if ($Root -eq '') {
  if (-not $env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set, so the nevr folder cannot be found; pass -Root <the player''s ...\AppData\Local\nevr\echovr>' }
  $Root = Join-Path (Join-Path $env:LOCALAPPDATA 'nevr') 'echovr'
}
if (-not [System.IO.Path]::IsPathRooted($Root)) { throw "-Root must be an absolute path (got $Root)" }
$target = Join-Path $Dir 'BugSplat64.dll'
$runtimeTarget = Join-Path $Root 'nevr-runtime.dll'
# The old layout: where a release before the split put the runtime. The host no longer looks there.
$legacyRuntime = Join-Path $Dir 'nevr-runtime.dll'
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
if (-not (Test-Path -LiteralPath $target)) { throw "no BugSplat64.dll in $Dir to back up" }
$state = Join-Path $Dir 'nevr-install.txt'
$originalSha = Get-Sha $target
# A NEVR runtime carries the NEVR-BUILD identity literal (#759). A runtime built before that literal (#785) carries
# the CDN user agent 'nevr-runtime/1.0' (src/runtime/patch/asset_cdn.cpp) instead, so either one marks a runtime.
function Test-NevrRuntime([string]$path) {
  $text = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($path))
  return $text.Contains('NEVR-BUILD ') -or $text.Contains('nevr-runtime/1.0')
}

# The state file: the set-aside names (the game's BugSplat64.dll, a legacy dbgcore.dll, a nevr-runtime.dll that was
# in the nevr folder before the first install, a nevr-runtime.dll moved out of the game folder), the folder the runtime
# was installed into, the recorded host and runtime hashes, and the host hashes of earlier builds.
function Write-State([string]$Backup, [string]$Legacy, $Previous, [string]$RuntimeAside, [string]$RuntimeLegacy) {
  $lines = @("backup=$Backup", "legacy=$Legacy", "runtime_aside=$RuntimeAside", "runtime_legacy=$RuntimeLegacy", "runtime_root=$Root", "installed_sha256=$ours", "runtime_sha256=$oursRuntime")
  if ($null -ne $Previous) { $lines += ('previous_sha256=' + $Previous) }
  # #770: UTF-8 without a BOM (Windows PowerShell 5.1 has no utf8NoBOM); -Encoding ASCII wrote a non-ASCII folder as '?'.
  $path = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($state)
  [System.IO.File]::WriteAllLines($path, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}
# Copy the package's runtime into the nevr folder (made when it is missing) and check the copy.
function Copy-Runtime {
  New-Item -ItemType Directory -Force -Path $Root | Out-Null
  Copy-Item -LiteralPath $runtimeDll -Destination $runtimeTarget -Force
  if ((Get-Sha $runtimeTarget) -ne $oursRuntime) { throw 'the installed nevr-runtime.dll does not match the package' }
}
# The old layout: a NEVR runtime beside echovr.exe is set aside whole as nevr-runtime.dll.legacy-<stamp> (never deleted;
# uninstall.ps1 renames it back). A file of that name that is not ours is not touched. $script:movedLegacy is the set-aside
# path, or '' when nothing was moved.
$script:movedLegacy = ''
function Move-LegacyRuntime {
  $script:movedLegacy = ''
  if (-not (Test-Path -LiteralPath $legacyRuntime)) { return }
  if (-not (Test-NevrRuntime $legacyRuntime)) {
    Write-Output "LEFT ${legacyRuntime}: it is not a NEVR runtime (no NEVR-BUILD identity text or nevr-runtime/1.0 user agent), so it is not an earlier release's"
    return
  }
  $aside = Join-Path $Dir ('nevr-runtime.dll.legacy-' + $stamp)
  if (Test-Path -LiteralPath $aside) { throw "name exists: ${aside}" }
  Rename-Item -LiteralPath $legacyRuntime -NewName (Split-Path -Leaf $aside)
  $script:movedLegacy = $aside
  Write-Output "MOVED the nevr-runtime.dll beside echovr.exe to $aside (the runtime lives in $Root now)"
}
function Test-LegacyRuntimePending { (Test-Path -LiteralPath $legacyRuntime) -and (Test-NevrRuntime $legacyRuntime) }

# #770: Verify Files restores the game's own BugSplat64.dll after an install, and the record then describes a file that is
# no longer there. A DLL with no NEVR-BUILD literal that the record does not list and that equals the recorded backup is the
# game's own file, so the record is stale: it is renamed aside (never deleted) and this run is a first install.
function Test-StaleRecord {
  $values = @{}
  foreach ($line in Get-Content -LiteralPath $state -Encoding UTF8) {
    if ($line -match '^([a-z_0-9]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
  }
  $known = @($values['installed_sha256'])
  if ($values.ContainsKey('previous_sha256') -and $values['previous_sha256'] -ne '') { $known += $values['previous_sha256'].Split(',') }
  if (($originalSha -eq $ours) -or ($known -contains $originalSha)) { return $false }
  # The game's own file is the recorded backup; a DLL that is neither recorded nor that backup is a stranger, and the
  # update path below refuses it.
  $recordedBackup = $values['backup']
  return ((-not [string]::IsNullOrEmpty($recordedBackup)) -and (Test-Path -LiteralPath $recordedBackup) -and ((Get-Sha $recordedBackup) -eq $originalSha) -and -not (Test-NevrRuntime $target))
}
if ((Test-Path -LiteralPath $state) -and (Test-StaleRecord)) {
  $staleAside = Join-Path $Dir ('nevr-install.txt.stale-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
  if (Test-Path -LiteralPath $staleAside) { throw "name exists: ${staleAside}" }
  Rename-Item -LiteralPath $state -NewName (Split-Path -Leaf $staleAside)
  Write-Output "STALE nevr-install.txt set aside as ${staleAside}: BugSplat64.dll is the game's own file and the record does not list it, so this is a first install"
  Write-RunLog ("NOTE $scriptName STALE nevr-install.txt set aside as $staleAside Dir=$Dir")
}

# #759: already installed means an update. The DLL must be one this folder's install put there (the recorded
# build or an earlier one); the original backup and the set-aside dbgcore.dll stay as the first install left
# them, so uninstall.ps1 still restores the oldest original. The new hash is recorded and the old ones kept.
if (Test-Path -LiteralPath $state) {
  $values = @{}
  foreach ($line in Get-Content -LiteralPath $state -Encoding UTF8) {
    if ($line -match '^([a-z_0-9]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
  }
  foreach ($key in 'backup', 'legacy', 'installed_sha256') {
    if (-not $values.ContainsKey($key)) { throw "nevr-install.txt has no $key (see $state)" }
  }
  # #781: both the recorded path and the update path depend on the backup and on no dbgcore.dll, so these checks run
  # before either branch writes anything; the recorded path would otherwise report success for an install uninstall.ps1 cannot undo.
  if (-not (Test-Path -LiteralPath $values['backup'])) { throw "the backup is gone: $($values['backup']); nothing was changed" }
  if (Test-Path -LiteralPath (Join-Path $Dir 'dbgcore.dll')) { throw 'a dbgcore.dll exists again; run uninstall.ps1 first' }
  # An update keeps what the first install set aside; a state file from before runtime_aside existed names none.
  $keptRuntimeAside = if ($values.ContainsKey('runtime_aside')) { $values['runtime_aside'] } else { '' }
  $keptRuntimeLegacy = if ($values.ContainsKey('runtime_legacy')) { $values['runtime_legacy'] } else { '' }
  # #779: an install made before #762 may have saved a NEVR runtime as the original; never restore or keep one.
  if ((Test-Path -LiteralPath $values['backup']) -and (Test-NevrRuntime $values['backup'])) {
    throw "the backup $($values['backup']) is a NEVR runtime, not the game's own file (an earlier install.ps1 saved it); nothing was changed. Restore the game's original BugSplat64.dll (for example, Verify Files in the Meta Horizon app), delete $state, then run install.ps1 again"
  }
  $known = @($values['installed_sha256'])
  if ($values.ContainsKey('previous_sha256') -and $values['previous_sha256'] -ne '') { $known += $values['previous_sha256'].Split(',') }
  if ($originalSha -eq $ours) {
    $runtimeCurrent = (Test-Path -LiteralPath $runtimeTarget) -and ((Get-Sha $runtimeTarget) -eq $oursRuntime)
    if ($values['installed_sha256'] -eq $ours -and $runtimeCurrent -and -not (Test-LegacyRuntimePending)) { throw 'the installed BugSplat64.dll and nevr-runtime.dll are already this package; nothing to do' }
    # This package's host DLL was copied in by hand (or its runtime is missing or another build): record the host, and put
    # this package's runtime in the nevr folder so the pair is one build.
    if (-not $runtimeCurrent) { Copy-Runtime }
    Move-LegacyRuntime
    $recordedLegacy = if ($keptRuntimeLegacy -ne '') { $keptRuntimeLegacy } else { $script:movedLegacy }
    $previous = @($known | Where-Object { $_ -ne $ours } | Select-Object -Unique)
    Write-State $values['backup'] $values['legacy'] ($previous -join ',') $keptRuntimeAside $recordedLegacy
    Write-Output "RECORDED BugSplat64.dll sha256=$ours as installed in $Dir and nevr-runtime.dll sha256=$oursRuntime as installed in $Root (the original backup is unchanged: $($values['backup']))"
    Write-RunLog ("OK $scriptName RECORDED BugSplat64.dll sha256=$ours nevr-runtime.dll sha256=$oursRuntime Dir=$Dir Root=$Root")
    Wait-BeforeClose
    exit 0
  }
  if ($known -notcontains $originalSha) {
    if (Test-NevrRuntime $target) {
      throw "BugSplat64.dll is a NEVR runtime install.ps1 did not record; run uninstall.ps1 first (it accepts any NEVR runtime), then install.ps1 again (see $state)"
    }
    throw "BugSplat64.dll is neither one install.ps1 put there nor a NEVR runtime, so uninstall.ps1 cannot undo it either; nothing was changed. Restore the game's own file (for example, Verify Files in the Meta Horizon app), delete $state, then run install.ps1 again"
  }
  # The runtime goes first: a host that is not yet replaced loads whichever runtime stands in the nevr folder, and a
  # failure puts both files, and a runtime moved out of the game folder, back as they were.
  $prior = [System.IO.File]::ReadAllBytes($target)
  $hadRuntime = Test-Path -LiteralPath $runtimeTarget
  $priorRuntime = if ($hadRuntime) { [System.IO.File]::ReadAllBytes($runtimeTarget) } else { $null }
  try {
    Copy-Runtime
    Copy-Item -LiteralPath $dll -Destination $target -Force
    if ((Get-Sha $target) -ne $ours) { throw 'the installed file does not match the package' }
    Move-LegacyRuntime
  } catch {
    Write-Output ("update failed: " + $_.Exception.Message + '; restoring')
    [System.IO.File]::WriteAllBytes($target, $prior)
    if ($hadRuntime) {
      [System.IO.File]::WriteAllBytes($runtimeTarget, $priorRuntime)
    } elseif (Test-Path -LiteralPath $runtimeTarget) {
      Rename-Item -LiteralPath $runtimeTarget -NewName ('nevr-runtime.dll.failed-' + $stamp)
    }
    if ($script:movedLegacy -ne '') { Rename-Item -LiteralPath $script:movedLegacy -NewName 'nevr-runtime.dll' }
    throw
  }
  $recordedLegacy = if ($keptRuntimeLegacy -ne '') { $keptRuntimeLegacy } else { $script:movedLegacy }
  $previous = @($known | Where-Object { $_ -ne $ours } | Select-Object -Unique)
  Write-State $values['backup'] $values['legacy'] ($previous -join ',') $keptRuntimeAside $recordedLegacy
  Write-Output "UPDATED BugSplat64.dll sha256=$ours into $Dir and nevr-runtime.dll sha256=$oursRuntime into $Root (the original backup is unchanged: $($values['backup']))"
  Write-RunLog ("OK $scriptName UPDATED BugSplat64.dll sha256=$ours nevr-runtime.dll sha256=$oursRuntime Dir=$Dir Root=$Root")
  Wait-BeforeClose
  exit 0
}

if ($originalSha -eq $ours) { throw 'the installed BugSplat64.dll is already this package; nothing to do' }
# The file about to be backed up as the original must be the game's own. A NEVR runtime carries the
# NEVR-BUILD identity literal; backing one up would make uninstall.ps1 "restore" a runtime (#759).
if (Test-NevrRuntime $target) {
  throw 'the installed BugSplat64.dll is a NEVR runtime, not the game''s own file, and nevr-install.txt is missing; the file was left as it is. Restore the game''s original BugSplat64.dll first (for example, Verify Files in the Meta Horizon app), then run install.ps1 again'
}

$backup = Join-Path $Dir ('BugSplat64.dll.original-' + $stamp)
if (Test-Path -LiteralPath $backup) { throw "backup name exists: ${backup}" }
Copy-Item -LiteralPath $target -Destination $backup
if ((Get-Sha $backup) -ne $originalSha) { throw "the backup does not match the original; nothing was changed ($backup is a bad copy: delete it by hand)" }
Write-Output ("original BugSplat64.dll sha256=$originalSha backed up to $backup")

# A legacy dbgcore.dll beside the runtime is fatal to it: set it aside, never delete it.
$legacy = Join-Path $Dir 'dbgcore.dll'
$legacyAside = ''
if (Test-Path -LiteralPath $legacy) {
  $legacyAside = Join-Path $Dir ('dbgcore.dll.legacy-' + (Get-Date).ToString('yyyyMMdd'))
  if (Test-Path -LiteralPath $legacyAside) { throw "name exists: ${legacyAside}" }
  Rename-Item -LiteralPath $legacy -NewName (Split-Path -Leaf $legacyAside)
  Write-Output "legacy dbgcore.dll renamed to $legacyAside"
}

# nevr-runtime.dll is not in a fresh nevr folder, so an existing one is another build's or a stranger's: set it aside
# like the original BugSplat64.dll, never overwrite it silently.
$runtimeAside = ''
if (Test-Path -LiteralPath $runtimeTarget) {
  $runtimeAside = Join-Path $Root ('nevr-runtime.dll.original-' + $stamp)
  if (Test-Path -LiteralPath $runtimeAside) { throw "name exists: ${runtimeAside}" }
  Rename-Item -LiteralPath $runtimeTarget -NewName (Split-Path -Leaf $runtimeAside)
  Write-Output "existing nevr-runtime.dll renamed to $runtimeAside"
}

try {
  # The runtime first: until the host is replaced nothing loads it.
  Copy-Runtime
  Copy-Item -LiteralPath $dll -Destination $target -Force
  if ((Get-Sha $target) -ne $ours) { throw 'the installed file does not match the package' }
  Move-LegacyRuntime
} catch {
  Write-Output ("install failed: " + $_.Exception.Message + '; restoring')
  Copy-Item -LiteralPath $backup -Destination $target -Force
  if (Test-Path -LiteralPath $runtimeTarget) {
    Rename-Item -LiteralPath $runtimeTarget -NewName ('nevr-runtime.dll.failed-' + $stamp)
  }
  if ($runtimeAside -ne '') { Rename-Item -LiteralPath $runtimeAside -NewName 'nevr-runtime.dll' }
  if ($legacyAside -ne '') { Rename-Item -LiteralPath $legacyAside -NewName 'dbgcore.dll' }
  if ($script:movedLegacy -ne '') { Rename-Item -LiteralPath $script:movedLegacy -NewName 'nevr-runtime.dll' }
  throw
}

# What uninstall.ps1 needs: the set-aside names and the nevr folder.
Write-State $backup $legacyAside $null $runtimeAside $script:movedLegacy
Write-Output "INSTALLED BugSplat64.dll sha256=$ours into $Dir and nevr-runtime.dll sha256=$oursRuntime into $Root"
Write-Output "To undo: powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 -Dir `"$Dir`""
Write-RunLog ("OK $scriptName INSTALLED BugSplat64.dll sha256=$ours nevr-runtime.dll sha256=$oursRuntime Dir=$Dir Root=$Root")

} catch {
  $exitCode = 1
  $message = $_.Exception.Message
  $shownDir = if ($Dir) { $Dir } else { '(not given)' }
  Write-Host ''
  Write-Host ('ERROR: ' + $message)
  Write-Host ('(also written to ' + $logFile + ')')
  Write-RunLog ($scriptName + ' ERROR: ' + $message + ' Dir=' + $shownDir)
}
Wait-BeforeClose
exit $exitCode
