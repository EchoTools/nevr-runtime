# NEVR runtime release: install.
#
# Puts this package's BugSplat64.dll into the Echo VR install and sets the old files aside. It never
# deletes anything: the original BugSplat64.dll is copied to BugSplat64.dll.original-<timestamp>, and a
# legacy dbgcore.dll is renamed to dbgcore.dll.legacy-<date>. uninstall.ps1 puts both back.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-Dir <...\ready-at-dawn-echo-arena\bin\win10>]
# Without -Dir the usual Meta Horizon / Oculus library locations are searched; with more than one
# install found, pass -Dir.
param([string]$Dir = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dll = Join-Path $here 'BugSplat64.dll'
$sums = Join-Path $here 'SHA256SUMS'
if (-not (Test-Path -LiteralPath $dll)) { throw "BugSplat64.dll is not next to install.ps1 ($here)" }
if (-not (Test-Path -LiteralPath $sums)) { throw "SHA256SUMS is not next to install.ps1 ($here)" }
$want = $null
foreach ($line in Get-Content -LiteralPath $sums) {
  if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$' -and $Matches[2] -eq 'BugSplat64.dll') { $want = $Matches[1].ToLower() }
}
if ($null -eq $want) { throw 'SHA256SUMS has no entry for BugSplat64.dll' }
$ours = Get-Sha $dll
if ($ours -ne $want) { throw "BugSplat64.dll does not match SHA256SUMS (got $ours): the package is damaged, download it again" }

if ($Dir -eq '') {
  $roots = @('C:\Program Files\Meta Horizon', 'C:\Program Files\Oculus', 'C:\Program Files (x86)\Oculus',
             'D:\Oculus', 'D:\Meta Horizon', 'E:\Oculus', 'E:\Meta Horizon')
  $found = @()
  foreach ($root in $roots) {
    $candidate = Join-Path $root 'Software\Software\ready-at-dawn-echo-arena\bin\win10'
    if (Test-Path -LiteralPath (Join-Path $candidate 'echovr.exe')) { $found += $candidate }
  }
  if ($found.Count -eq 0) { throw 'Echo VR was not found in the usual places; pass -Dir <...\bin\win10>' }
  if ($found.Count -gt 1) { throw ('more than one Echo VR install found; pass -Dir with one of: ' + ($found -join '; ')) }
  $Dir = $found[0]
}
if (-not (Test-Path -LiteralPath (Join-Path $Dir 'echovr.exe'))) { throw "no echovr.exe in $Dir" }
if (@(Get-Process -Name echovr -ErrorAction SilentlyContinue).Count -gt 0) { throw 'echovr.exe is running; close the game first' }

$target = Join-Path $Dir 'BugSplat64.dll'
if (-not (Test-Path -LiteralPath $target)) { throw "no BugSplat64.dll in $Dir to back up" }
$state = Join-Path $Dir 'nevr-install.txt'
if (Test-Path -LiteralPath $state) { throw "already installed (see $state); run uninstall.ps1 first" }
$originalSha = Get-Sha $target
if ($originalSha -eq $ours) { throw 'the installed BugSplat64.dll is already this package; nothing to do' }

$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
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

try {
  Copy-Item -LiteralPath $dll -Destination $target -Force
  if ((Get-Sha $target) -ne $ours) { throw 'the installed file does not match the package' }
} catch {
  Write-Output ("install failed: " + $_.Exception.Message + '; restoring')
  Copy-Item -LiteralPath $backup -Destination $target -Force
  if ($legacyAside -ne '') { Rename-Item -LiteralPath $legacyAside -NewName 'dbgcore.dll' }
  throw
}

# What uninstall.ps1 needs: both set-aside names.
Set-Content -LiteralPath $state -Encoding ASCII -Value @("backup=$backup", "legacy=$legacyAside", "installed_sha256=$ours")
Write-Output "INSTALLED BugSplat64.dll sha256=$ours into $Dir"
Write-Output "To undo: powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 -Dir `"$Dir`""
