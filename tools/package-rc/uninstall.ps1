# NEVR runtime release candidate: uninstall.
#
# Puts back what install.ps1 set aside: the original BugSplat64.dll and a legacy dbgcore.dll. It never
# deletes anything: the backup stays where it is, and nothing is overwritten that was not put there by
# install.ps1.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 -Dir <...\bin\win10>
param([Parameter(Mandatory = $true)][string]$Dir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }

if (@(Get-Process -Name echovr -ErrorAction SilentlyContinue).Count -gt 0) { throw 'echovr.exe is running; close the game first' }
$state = Join-Path $Dir 'nevr-rc-install.txt'
if (-not (Test-Path -LiteralPath $state)) { throw "no nevr-rc-install.txt in ${Dir}: nothing installed by install.ps1 to undo" }
$values = @{}
foreach ($line in Get-Content -LiteralPath $state) {
  if ($line -match '^([a-z_0-9]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
}
foreach ($key in 'backup', 'legacy', 'installed_sha256') {
  if (-not $values.ContainsKey($key)) { throw "nevr-rc-install.txt has no $key" }
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
Rename-Item -LiteralPath $state -NewName ('nevr-rc-install.txt.undone-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
