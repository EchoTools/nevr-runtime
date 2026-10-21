# NEVR runtime release: uninstall.
#
# Puts back what install.ps1 set aside: the original BugSplat64.dll and a legacy dbgcore.dll. It never
# deletes anything: the backup stays where it is, and nothing is overwritten that was not put there by
# install.ps1.
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
  Read-Host 'Press Enter to close' | Out-Null
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
$target = Join-Path $Dir 'BugSplat64.dll'
if ((Test-Path -LiteralPath $target) -and ((Get-Sha $target) -ne $values['installed_sha256'])) {
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
Rename-Item -LiteralPath $state -NewName ('nevr-install.txt.undone-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
Write-RunLog ("OK $scriptName RESTORED BugSplat64.dll Dir=$Dir")

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
