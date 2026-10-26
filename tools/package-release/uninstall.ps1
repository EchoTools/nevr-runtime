# NEVR runtime release: uninstall.
#
# Puts back what install.ps1 set aside: the original BugSplat64.dll and a legacy dbgcore.dll, and sets
# nevr-runtime.dll, the file install.ps1 added, aside as nevr-runtime.dll.uninstalled-<timestamp>. It never
# deletes anything: the backup stays where it is, and nothing is overwritten that was not put there by
# install.ps1. A nevr-runtime.dll that is not a NEVR runtime (it carries no NEVR-BUILD identity text) is left
# alone and reported.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 [-Dir <...\bin\win10>] [-Pause | -NoPause]
# Without -Dir the folder is asked for.
param([string]$Dir = '', [switch]$Pause, [switch]$NoPause)
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
if ($Dir -eq '') {
  $answer = Read-Host 'Type the folder that holds echovr.exe (...\ready-at-dawn-echo-arena\bin\win10)'
  if ($null -ne $answer) { $answer = $answer.Trim().Trim('"') }
  if (-not $answer) { throw 'no folder given; pass -Dir <...\bin\win10>' }
  $Dir = $answer
}

if (@(Get-Process -Name echovr -ErrorAction SilentlyContinue).Count -gt 0) { throw 'echovr.exe is running; close the game first' }
$state = Join-Path $Dir 'nevr-install.txt'
if (-not (Test-Path -LiteralPath $state)) { throw "no nevr-install.txt in ${Dir}: nothing installed by install.ps1 to undo" }
$values = @{}
foreach ($line in Get-Content -LiteralPath $state) {
  if ($line -match '^([a-z_0-9]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
}
foreach ($key in 'backup', 'legacy', 'installed_sha256') {
  if (-not $values.ContainsKey($key)) { throw "nevr-install.txt has no $key" }
}
$backup = $values['backup']
$legacyAside = $values['legacy']
if (-not (Test-Path -LiteralPath $backup)) { throw "the backup is gone: ${backup}; nothing was changed" }
# #779: an install made before #762 may have saved a NEVR runtime as the original; restoring it would restore the runtime.
if ([System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($backup)).Contains('NEVR-BUILD ')) {
  throw "the backup ${backup} is a NEVR runtime, not the game's own file (an earlier install.ps1 saved it); nothing was changed. Restore the game's original BugSplat64.dll (for example, Verify Files in the Meta Horizon app), then delete $state"
}
$target = Join-Path $Dir 'BugSplat64.dll'
# #759: the DLL must be a build of ours: the one recorded, an earlier one install.ps1 recorded when it updated,
# or the one in the package this script came with (a DLL copied in by hand from a newer package).
$known = @($values['installed_sha256'])
if ($values.ContainsKey('previous_sha256') -and $values['previous_sha256'] -ne '') { $known += $values['previous_sha256'].Split(',') }
$sums = Join-Path $here 'SHA256SUMS'
if (Test-Path -LiteralPath $sums) {
  foreach ($line in Get-Content -LiteralPath $sums) {
    if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$' -and $Matches[2] -eq 'BugSplat64.dll') { $known += $Matches[1].ToLower() }
  }
}
# A DLL carrying the NEVR-BUILD identity literal is a NEVR runtime: putting the original back over it is safe.
$isRuntime = (Test-Path -LiteralPath $target) -and [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($target)).Contains('NEVR-BUILD ')
if ((Test-Path -LiteralPath $target) -and ($known -notcontains (Get-Sha $target)) -and -not $isRuntime) {
  throw 'BugSplat64.dll is not the one install.ps1 put there (changed since); nothing was changed'
}
if ($legacyAside -ne '') {
  if (-not (Test-Path -LiteralPath $legacyAside)) { throw "the set-aside dbgcore.dll is gone: ${legacyAside}; nothing was changed" }
  if (Test-Path -LiteralPath (Join-Path $Dir 'dbgcore.dll')) { throw 'a dbgcore.dll exists again; nothing was changed' }
}

Copy-Item -LiteralPath $backup -Destination $target -Force
if ((Get-Sha $target) -ne (Get-Sha $backup)) { throw 'the restored BugSplat64.dll does not match the backup' }
Write-Output ("RESTORED BugSplat64.dll sha256=" + (Get-Sha $target) + " from $backup (backup kept)")
if ($legacyAside -ne '') {
  Rename-Item -LiteralPath $legacyAside -NewName 'dbgcore.dll'
  Write-Output 'RESTORED dbgcore.dll'
}
# The game no longer loads the runtime: set it aside, if it is ours.
$runtimeTarget = Join-Path $Dir 'nevr-runtime.dll'
$runtimeRemoved = $false
if (Test-Path -LiteralPath $runtimeTarget) {
  if ([System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($runtimeTarget)).Contains('NEVR-BUILD ')) {
    $runtimeAside = 'nevr-runtime.dll.uninstalled-' + (Get-Date).ToString('yyyyMMdd-HHmmss')
    Rename-Item -LiteralPath $runtimeTarget -NewName $runtimeAside
    $runtimeRemoved = $true
    Write-Output "SET ASIDE nevr-runtime.dll as $runtimeAside"
  } else {
    Write-Output 'LEFT nevr-runtime.dll: it is not a NEVR runtime (no NEVR-BUILD identity text), so install.ps1 did not put it there'
  }
}
Rename-Item -LiteralPath $state -NewName ('nevr-install.txt.undone-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
Write-RunLog ("OK $scriptName RESTORED BugSplat64.dll Dir=$Dir nevr-runtime.dll set aside=$runtimeRemoved")

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
