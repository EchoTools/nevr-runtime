# NEVR runtime release: uninstall.
#
# Puts back what install.ps1 set aside: the original BugSplat64.dll and a legacy dbgcore.dll, and sets
# nevr-runtime.dll, the file install.ps1 added to the player's nevr folder (%LOCALAPPDATA%\nevr\echovr, ADR 0012), aside
# as nevr-runtime.dll.uninstalled-<timestamp>. Nothing of ours is written to the game folder: BugSplat64.dll is the only
# file of ours that sits there. The install record is <nevr folder>\installer\nevr-install.txt and every file this script
# sets aside goes to <nevr folder>\installer\aside; a record an earlier install.ps1 left beside echovr.exe is read when
# the nevr folder has none, and is moved into the aside folder when the uninstall ends. A nevr-runtime.dll that install.ps1
# had set aside there (nevr-runtime.dll.original-<stamp>, named by runtime_aside) goes back under its own name, and so does
# one it moved out of the game folder (nevr-runtime.dll.legacy-<stamp>, named by runtime_legacy), into the game folder. It
# never deletes anything of the player's: the backup stays where it is, and nothing is overwritten that was not put there
# by install.ps1. A nevr-runtime.dll that is not a NEVR runtime (it carries neither the NEVR-BUILD identity text nor the
# CDN user agent 'nevr-runtime/1.0', #785) is left alone and reported.
#
# The sign-in cache stays where the runtime keeps it, in <nevr folder> (.credentials.json), and uninstall writes nothing
# into the game folder's _local: a runtime that still reads _local only starts a new sign-in.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 [-Dir <...\bin\win10>] [-Root <nevr folder>] [-Pause | -NoPause]
# Without -Dir the folder is asked for. -Root names the nevr folder when it is not %LOCALAPPDATA%\nevr\echovr (an
# administrator's prompt); the record of an install made by this release names it too (runtime_root).
param([string]$Dir = '', [string]$Root = '', [switch]$Pause, [switch]$NoPause)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }
# A NEVR runtime carries the NEVR-BUILD identity literal (#759); one built before that literal (#785) carries the CDN
# user agent 'nevr-runtime/1.0' (src/runtime/patch/asset_cdn.cpp). Either one marks a runtime.
function Test-NevrRuntime([string]$path) {
  $text = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($path))
  return $text.Contains('NEVR-BUILD ') -or $text.Contains('nevr-runtime/1.0')
}

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
if ($Dir -eq '') {
  $answer = Read-Host 'Type the folder that holds echovr.exe (...\ready-at-dawn-echo-arena\bin\win10)'
  if ($null -ne $answer) { $answer = $answer.Trim().Trim('"') }
  if (-not $answer) { throw 'no folder given; pass -Dir <...\bin\win10>' }
  $Dir = $answer
}

if (@(Get-Process -Name echovr -ErrorAction SilentlyContinue).Count -gt 0) { throw 'echovr.exe is running; close the game first' }
# The record: <nevr folder>\installer\nevr-install.txt; a record beside echovr.exe (an earlier install.ps1) is read when the
# nevr folder has none. The nevr folder is -Root, else %LOCALAPPDATA%\nevr\echovr, else the one the legacy record names.
$nevrRoot = $Root
if ($nevrRoot -eq '' -and $env:LOCALAPPDATA) { $nevrRoot = Join-Path (Join-Path $env:LOCALAPPDATA 'nevr') 'echovr' }
$legacyState = Join-Path $Dir 'nevr-install.txt'
$state = ''
if ($nevrRoot -ne '') {
  $rootState = Join-Path (Join-Path $nevrRoot 'installer') 'nevr-install.txt'
  if (Test-Path -LiteralPath $rootState) { $state = $rootState }
}
if ($state -eq '' -and (Test-Path -LiteralPath $legacyState)) { $state = $legacyState }
if ($state -eq '') { throw "no nevr-install.txt in $(if ($nevrRoot -ne '') { Join-Path $nevrRoot 'installer' } else { 'the nevr folder' }) or in ${Dir}: nothing installed by install.ps1 to undo" }
$values = @{}
foreach ($line in Get-Content -LiteralPath $state -Encoding UTF8) {
  if ($line -match '^([a-z_0-9]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
}
foreach ($key in 'backup', 'legacy', 'installed_sha256') {
  if (-not $values.ContainsKey($key)) { throw "$state has no $key" }
}
if ($nevrRoot -eq '' -and $values.ContainsKey('runtime_root') -and $values['runtime_root'] -ne '') { $nevrRoot = $values['runtime_root'] }
if ($nevrRoot -eq '') { throw 'the nevr folder is unknown (LOCALAPPDATA is not set); pass -Root <nevr folder>' }
# Every file this script sets aside lands here, never in the game folder.
$asideDir = Join-Path (Join-Path $nevrRoot 'installer') 'aside'
$backup = $values['backup']
$legacyAside = $values['legacy']
# A nevr-runtime.dll that stood in the folder before install.ps1 (install.ps1 renamed it to nevr-runtime.dll.original-<stamp>);
# a state file from before this key existed names none.
$priorRuntimeAside = if ($values.ContainsKey('runtime_aside')) { $values['runtime_aside'] } else { '' }
# A nevr-runtime.dll that install.ps1 moved out of the game folder (the old layout) to nevr-runtime.dll.legacy-<stamp>.
$movedRuntimeAside = if ($values.ContainsKey('runtime_legacy')) { $values['runtime_legacy'] } else { '' }
# Where the runtime was installed: -Root, else the folder the record names, else (a record from before the split) beside echovr.exe.
$runtimeRoot = if ($Root -ne '') { $Root } elseif ($values.ContainsKey('runtime_root') -and $values['runtime_root'] -ne '') { $values['runtime_root'] } else { $Dir }
if (-not (Test-Path -LiteralPath $backup)) { throw "the backup is gone: ${backup}; nothing was changed" }
# #779: an install made before #762 may have saved a NEVR runtime as the original; restoring it would restore the runtime.
if (Test-NevrRuntime $backup) {
  throw "the backup ${backup} is a NEVR runtime, not the game's own file (an earlier install.ps1 saved it); nothing was changed. Restore the game's original BugSplat64.dll (for example, Verify Files in the Meta Horizon app), then set $state aside"
}
$target = Join-Path $Dir 'BugSplat64.dll'
# #759: the DLL must be a build of ours: the one recorded, an earlier one install.ps1 recorded when it updated,
# or the one in the package this script came with (a DLL copied in by hand from a newer package).
$known = @($values['installed_sha256'])
if ($values.ContainsKey('previous_sha256') -and $values['previous_sha256'] -ne '') { $known += $values['previous_sha256'].Split(',') }
# #770: after Verify Files the game's own file is back, and it is the backup: already restored, so nothing is overwritten.
$known += Get-Sha $backup
$sums = Join-Path $here 'SHA256SUMS'
if (Test-Path -LiteralPath $sums) {
  foreach ($line in Get-Content -LiteralPath $sums) {
    if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$' -and $Matches[2] -eq 'BugSplat64.dll') { $known += $Matches[1].ToLower() }
  }
}
# A DLL carrying the NEVR-BUILD identity literal is a NEVR runtime: putting the original back over it is safe.
$isRuntime = (Test-Path -LiteralPath $target) -and (Test-NevrRuntime $target)
if ((Test-Path -LiteralPath $target) -and ($known -notcontains (Get-Sha $target)) -and -not $isRuntime) {
  throw 'BugSplat64.dll is not the one install.ps1 put there (changed since); nothing was changed'
}
if ($legacyAside -ne '') {
  if (-not (Test-Path -LiteralPath $legacyAside)) { throw "the set-aside dbgcore.dll is gone: ${legacyAside}; nothing was changed" }
  if (Test-Path -LiteralPath (Join-Path $Dir 'dbgcore.dll')) { throw 'a dbgcore.dll exists again; nothing was changed' }
}
if (($priorRuntimeAside -ne '') -and -not (Test-Path -LiteralPath $priorRuntimeAside)) {
  throw "the set-aside nevr-runtime.dll is gone: ${priorRuntimeAside}; nothing was changed"
}
if (($movedRuntimeAside -ne '') -and -not (Test-Path -LiteralPath $movedRuntimeAside)) {
  throw "the set-aside nevr-runtime.dll is gone: ${movedRuntimeAside}; nothing was changed"
}

Copy-Item -LiteralPath $backup -Destination $target -Force
if ((Get-Sha $target) -ne (Get-Sha $backup)) { throw 'the restored BugSplat64.dll does not match the backup' }
Write-Output ("RESTORED BugSplat64.dll sha256=" + (Get-Sha $target) + " from $backup (backup kept)")
if ($legacyAside -ne '') {
  Move-Item -LiteralPath $legacyAside -Destination (Join-Path $Dir 'dbgcore.dll')
  Write-Output 'RESTORED dbgcore.dll'
}
# The game no longer loads the runtime: set it aside, if it is ours.
$runtimeTarget = Join-Path $runtimeRoot 'nevr-runtime.dll'
$runtimeRemoved = $false
if (Test-Path -LiteralPath $runtimeTarget) {
  if (Test-NevrRuntime $runtimeTarget) {
    $runtimeAside = 'nevr-runtime.dll.uninstalled-' + (Get-Date).ToString('yyyyMMdd-HHmmss')
    New-Item -ItemType Directory -Force -Path $asideDir | Out-Null
    Move-Item -LiteralPath $runtimeTarget -Destination (Join-Path $asideDir $runtimeAside)
    $runtimeRemoved = $true
    Write-Output "SET ASIDE nevr-runtime.dll as $runtimeAside in $asideDir"
  } else {
    Write-Output 'LEFT nevr-runtime.dll: it is not a NEVR runtime (no NEVR-BUILD identity text or nevr-runtime/1.0 user agent), so install.ps1 did not put it there'
  }
}
# Then the file that stood there before install.ps1 goes back, when the name is free; it is never put over another file.
$runtimeRestored = $false
if ($priorRuntimeAside -ne '') {
  if (Test-Path -LiteralPath $runtimeTarget) {
    Write-Output "KEPT ${priorRuntimeAside}: nevr-runtime.dll is in the folder and was not touched, so the earlier file stays aside"
  } else {
    Move-Item -LiteralPath $priorRuntimeAside -Destination $runtimeTarget
    $runtimeRestored = $true
    Write-Output "RESTORED nevr-runtime.dll from ${priorRuntimeAside}"
  }
}
# The nevr-runtime.dll install.ps1 moved out of the game folder goes back there, when the name is free.
$legacyRuntimeTarget = Join-Path $Dir 'nevr-runtime.dll'
$movedRestored = $false
if ($movedRuntimeAside -ne '') {
  if (Test-Path -LiteralPath $legacyRuntimeTarget) {
    Write-Output "KEPT ${movedRuntimeAside}: nevr-runtime.dll is in the game folder and was not touched, so the earlier file stays aside"
  } else {
    Move-Item -LiteralPath $movedRuntimeAside -Destination $legacyRuntimeTarget
    $movedRestored = $true
    Write-Output "RESTORED nevr-runtime.dll from ${movedRuntimeAside}"
  }
}
# The record is set aside under the nevr folder; one read from beside echovr.exe leaves the game folder this way.
New-Item -ItemType Directory -Force -Path $asideDir | Out-Null
$undoneRecord = Join-Path $asideDir ('nevr-install.txt.undone-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
Move-Item -LiteralPath $state -Destination $undoneRecord
Write-RunLog ("OK $scriptName RESTORED BugSplat64.dll Dir=$Dir nevr-runtime.dll set aside=$runtimeRemoved earlier nevr-runtime.dll restored=$runtimeRestored moved nevr-runtime.dll restored=$movedRestored runtime folder=$runtimeRoot")

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
