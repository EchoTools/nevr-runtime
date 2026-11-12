# Self-test of probe_install.ps1 / probe_restore.ps1 against a fake nevr folder and game folder in a fresh scratch dir.
# The real ones are never written: the children run with LOCALAPPDATA pointed into the scratch dir and -Dir always names
# the fake game, and the test checks afterwards that the real nevr folder still has no probe backup. Run it while the
# game is closed; it starts a renamed ping.exe as a fake echovr.exe for a few seconds.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File tools\maps_probes\test_probe_install.ps1 [-Scratch <new dir>] [-ScriptDir <dir with the scripts>]
param([string]$Scratch = '', [string]$ScriptDir = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
if ($ScriptDir -eq '') { $ScriptDir = $PSScriptRoot }
if ($Scratch -eq '') { $Scratch = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), 'probe-install-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8)) }
$Scratch = [System.IO.Path]::GetFullPath($Scratch)
if ([System.IO.Directory]::Exists($Scratch)) { throw "$Scratch exists; the self-test needs a new folder" }
if (@(Get-Process -Name echovr -ErrorAction SilentlyContinue).Count -gt 0) { throw 'echovr.exe is running; close it before the self-test' }

$realLocal = $env:LOCALAPPDATA
$realInstaller = if ($realLocal) { [System.IO.Path]::Combine($realLocal, 'nevr', 'echovr', 'installer') } else { '' }
function Get-RealBackupCount {
  if ($realInstaller -eq '' -or -not [System.IO.Directory]::Exists($realInstaller)) { return 0 }
  return @(Get-ChildItem -LiteralPath $realInstaller -Directory -Filter 'probe-backup-*').Count
}
$realBackupsBefore = Get-RealBackupCount

$latin1 = [System.Text.Encoding]::GetEncoding(28591)
function Write-Fake([string]$Path, [string]$Text) {
  [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path)) | Out-Null
  [System.IO.File]::WriteAllText($Path, $Text, $latin1)
}
function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }

$pkg = [System.IO.Path]::Combine($Scratch, 'pkg')
$local = [System.IO.Path]::Combine($Scratch, 'LocalAppData')
$root = [System.IO.Path]::Combine($local, 'nevr', 'echovr')
$game = [System.IO.Path]::Combine($Scratch, 'Games', 'ready-at-dawn-echo-arena', 'bin', 'win10')
$watched = @($local, [System.IO.Path]::Combine($Scratch, 'Games'))

$releaseRuntime = "MZ release runtime`0NEVR-BUILD 5.4.3 0123456789abcdef0123456789abcdef01234567`0NEVR-PLUGIN-TRUST release`0"
$releaseHost = "MZ release host`0NEVR-BUILD 5.4.3 0123456789abcdef0123456789abcdef01234567`0NEVR-HOST-BUILD release`0"
$probeRuntime = "MZ probe runtime`0NEVR-BUILD 5.4.3-probe 89abcdef0123456789abcdef0123456789abcdef`0NEVR-PLUGIN-TRUST dev`0"
$probeHost = "MZ probe host`0NEVR-BUILD 5.4.3-probe 89abcdef0123456789abcdef0123456789abcdef`0NEVR-HOST-BUILD dev`0"

function Set-Package([string]$HostText, [string]$RuntimeText, [switch]$BadSum) {
  Write-Fake ([System.IO.Path]::Combine($pkg, 'BugSplat64.dll')) $HostText
  Write-Fake ([System.IO.Path]::Combine($pkg, 'nevr-runtime.dll')) $RuntimeText
  $sums = @()
  foreach ($name in 'nevr-runtime.dll', 'BugSplat64.dll', 'maps_probe.yaml') {
    $sums += (Get-Sha ([System.IO.Path]::Combine($pkg, $name))) + '  ' + $name
  }
  [System.IO.File]::WriteAllLines([System.IO.Path]::Combine($pkg, 'PROBE_SHA256SUMS'), [string[]]$sums)
  if ($BadSum) { Write-Fake ([System.IO.Path]::Combine($pkg, 'nevr-runtime.dll')) ($RuntimeText + 'x') }
}

[System.IO.Directory]::CreateDirectory($pkg) | Out-Null
foreach ($name in 'probe_install.ps1', 'probe_restore.ps1', 'probe_common.ps1', 'maps_probe.yaml') {
  [System.IO.File]::Copy([System.IO.Path]::Combine($ScriptDir, $name), [System.IO.Path]::Combine($pkg, $name))
}
Set-Package $probeHost $probeRuntime
Write-Fake ([System.IO.Path]::Combine($root, 'nevr-runtime.dll')) $releaseRuntime
Write-Fake ([System.IO.Path]::Combine($root, 'config.yaml')) "channel: stable`r`n"
Write-Fake ([System.IO.Path]::Combine($root, 'config.json')) '{"a":1}'
Write-Fake ([System.IO.Path]::Combine($root, 'logs', 'nevr-20261112T000000Z.jsonl')) "{}`n"
Write-Fake ([System.IO.Path]::Combine($root, 'installer', 'nevr-install.txt')) "owner=nevr-install`r`n"
Write-Fake ([System.IO.Path]::Combine($game, 'echovr.exe')) 'MZ game'
Write-Fake ([System.IO.Path]::Combine($game, 'BugSplat64.dll')) $releaseHost
Write-Fake ([System.IO.Path]::Combine($game, 'config.json')) '{"game":1}'
Write-Fake ([System.IO.Path]::Combine($game, '..', '..', '_local', 'config.json')) '{"local":1}'

function Get-Snapshot {
  $map = @{}
  foreach ($dir in $watched) {
    foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -File) { $map[$f.FullName.Substring($Scratch.Length + 1)] = Get-Sha $f.FullName }
  }
  return $map
}
# The paths that differ between two snapshots, sorted.
function Compare-Snapshot($A, $B) {
  $changed = @()
  foreach ($k in $A.Keys) { if (-not $B.ContainsKey($k) -or $B[$k] -ne $A[$k]) { $changed += $k } }
  foreach ($k in $B.Keys) { if (-not $A.ContainsKey($k)) { $changed += $k } }
  return @($changed | Sort-Object)
}

$script:failures = 0
function Check([bool]$Ok, [string]$Name) {
  if ($Ok) { Write-Output "PASS $Name" } else { Write-Output "FAIL $Name"; $script:failures++ }
}
$script:lastOutput = @()
function Invoke-Probe([string]$Script, [string[]]$Arguments) {
  $env:LOCALAPPDATA = $local
  try {
    $script:lastOutput = @(& powershell -NoProfile -ExecutionPolicy Bypass -File ([System.IO.Path]::Combine($pkg, $Script)) @Arguments)
    return $LASTEXITCODE
  } finally {
    $env:LOCALAPPDATA = $realLocal
  }
}
function Test-Says([string]$Text) { return (($script:lastOutput -join "`n").Contains($Text)) }
function Test-NoPathsShown { return -not (($script:lastOutput -join "`n").Contains($Scratch)) }
function Test-Unchanged($Before) { return @(Compare-Snapshot $Before (Get-Snapshot)).Count -eq 0 }
function Get-BackupDirs { @(Get-ChildItem -LiteralPath ([System.IO.Path]::Combine($root, 'installer')) -Directory -Filter 'probe-backup-*' | Sort-Object Name) }

$fakeGame = $null
function Start-FakeGame {
  $exe = [System.IO.Path]::Combine($Scratch, 'proc', 'echovr.exe')
  [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($exe)) | Out-Null
  if (-not [System.IO.File]::Exists($exe)) { [System.IO.File]::Copy([System.IO.Path]::Combine($env:SystemRoot, 'System32', 'PING.EXE'), $exe) }
  $script:fakeGame = Start-Process -FilePath $exe -ArgumentList '-n', '60', '127.0.0.1' -WindowStyle Hidden -PassThru
  for ($i = 0; $i -lt 50 -and @(Get-Process -Name echovr -ErrorAction SilentlyContinue).Count -eq 0; $i++) { Start-Sleep -Milliseconds 100 }
}
function Stop-FakeGame { if ($script:fakeGame) { Stop-Process -Id $script:fakeGame.Id -Force; $script:fakeGame.WaitForExit(); $script:fakeGame = $null } }

$gameArgs = @('-Dir', $game)
try {
  $original = Get-Snapshot

  # Refusals before anything is written.
  Set-Package $releaseHost $probeRuntime
  Check ((Invoke-Probe 'probe_install.ps1' $gameArgs) -eq 1 -and (Test-Says 'not a dev host') -and (Test-Unchanged $original)) 'install refuses a package whose host is a release host'
  Set-Package $probeHost $probeRuntime -BadSum
  Check ((Invoke-Probe 'probe_install.ps1' $gameArgs) -eq 1 -and (Test-Says 'damaged') -and (Test-Unchanged $original)) 'install refuses a package that does not match PROBE_SHA256SUMS'
  Set-Package $probeHost $probeRuntime
  Write-Fake ([System.IO.Path]::Combine($game, 'BugSplat64.dll')) 'MZ BugSplat stock'
  $stock = Get-Snapshot
  Check ((Invoke-Probe 'probe_install.ps1' $gameArgs) -eq 1 -and (Test-Says 'not a nEVR host') -and (Test-Unchanged $stock)) 'install refuses a game folder without a nEVR host'
  Write-Fake ([System.IO.Path]::Combine($game, 'BugSplat64.dll')) $releaseHost
  Check ((Invoke-Probe 'probe_install.ps1' @('-Dir', $game, '-Root', [System.IO.Path]::Combine($Scratch, 'nowhere'))) -eq 1 -and (Test-Says 'not a nevr install') -and (Test-Unchanged $original)) 'install refuses a nevr folder with no nEVR runtime'
  Check ((Invoke-Probe 'probe_install.ps1' @('-Dir', $game, '-Root', 'relative\nevr')) -eq 1 -and (Test-Unchanged $original)) 'install refuses a relative -Root'
  $common = [System.IO.Path]::Combine($pkg, 'probe_common.ps1').Replace("'", "''")
  $copyFrom = [System.IO.Path]::Combine($pkg, 'nevr-runtime.dll').Replace("'", "''")
  $copyTo = [System.IO.Path]::Combine($root, 'nevr-runtime.dll').Replace("'", "''")
  $said = @(& powershell -NoProfile -ExecutionPolicy Bypass -Command ". '$common'; try { Copy-Verified '$copyFrom' '$copyTo' 'bad'; 'copied' } catch { 'refused' }")
  Check ((($said -join '') -eq 'refused') -and (Test-Unchanged $original)) 'a copy that reads back wrong leaves its target and no partial file'
  Check ((Invoke-Probe 'probe_install.ps1' ($gameArgs + '-WhatIf')) -eq 0 -and (Test-Says '-WhatIf: nothing was written') -and (Test-Says 'replace  game folder\BugSplat64.dll') -and (Test-Says 'add      nevr folder\maps_probe.yaml') -and (Test-Unchanged $original)) 'install -WhatIf plans and writes nothing'
  Start-FakeGame
  Check ((Invoke-Probe 'probe_install.ps1' $gameArgs) -eq 1 -and (Test-Says 'echovr.exe is running') -and (Test-Unchanged $original)) 'install refuses while echovr.exe runs'
  Stop-FakeGame

  # The install, into the default nevr folder (LOCALAPPDATA), touches exactly the three files and its backup.
  Check ((Invoke-Probe 'probe_install.ps1' $gameArgs) -eq 0) 'install succeeds'
  Check (Test-NoPathsShown) 'install output names no full path'
  $installed = Get-Snapshot
  $backups = @(Get-BackupDirs)
  Check ($backups.Count -eq 1) 'install made one backup folder'
  $b = 'LocalAppData\nevr\echovr\installer\' + $backups[0].Name + '\'
  $expected = @('Games\ready-at-dawn-echo-arena\bin\win10\BugSplat64.dll', ($b + 'game-BugSplat64.dll'), ($b + 'manifest.tsv'),
    ($b + 'root-nevr-runtime.dll'), 'LocalAppData\nevr\echovr\maps_probe.yaml', 'LocalAppData\nevr\echovr\nevr-runtime.dll') | Sort-Object
  Check (((Compare-Snapshot $original $installed) -join '|') -eq ($expected -join '|')) 'install changed only the three files and added only its backup'
  Check ($installed['LocalAppData\nevr\echovr\nevr-runtime.dll'] -eq (Get-Sha ([System.IO.Path]::Combine($pkg, 'nevr-runtime.dll'))) -and
    $installed['Games\ready-at-dawn-echo-arena\bin\win10\BugSplat64.dll'] -eq (Get-Sha ([System.IO.Path]::Combine($pkg, 'BugSplat64.dll'))) -and
    $installed['LocalAppData\nevr\echovr\maps_probe.yaml'] -eq (Get-Sha ([System.IO.Path]::Combine($pkg, 'maps_probe.yaml')))) 'installed files are the package files'
  Check ($installed[$b + 'root-nevr-runtime.dll'] -eq $original['LocalAppData\nevr\echovr\nevr-runtime.dll'] -and
    $installed[$b + 'game-BugSplat64.dll'] -eq $original['Games\ready-at-dawn-echo-arena\bin\win10\BugSplat64.dll']) 'backup holds the replaced files'

  Check ((Invoke-Probe 'probe_install.ps1' $gameArgs) -eq 1 -and (Test-Says 'already in place') -and (Test-Unchanged $installed)) 'a second install is refused until a restore'
  Start-FakeGame
  Check ((Invoke-Probe 'probe_restore.ps1' @()) -eq 1 -and (Test-Says 'echovr.exe is running') -and (Test-Unchanged $installed)) 'restore refuses while echovr.exe runs'
  Stop-FakeGame
  Check ((Invoke-Probe 'probe_restore.ps1' @('-WhatIf')) -eq 0 -and (Test-Says 'put back nevr folder\nevr-runtime.dll') -and (Test-Says 'remove   nevr folder\maps_probe.yaml') -and (Test-Unchanged $installed)) 'restore -WhatIf plans and writes nothing'

  $runtimePath = [System.IO.Path]::Combine($root, 'nevr-runtime.dll')
  Write-Fake $runtimePath ($releaseRuntime + 'newer')
  $tampered = Get-Snapshot
  Check ((Invoke-Probe 'probe_restore.ps1' @()) -eq 1 -and (Test-Says 'changed since the probe install') -and (Test-Unchanged $tampered)) 'restore refuses when a probe DLL was replaced since'
  Write-Fake $runtimePath $probeRuntime
  $savedHost = [System.IO.Path]::Combine($backups[0].FullName, 'game-BugSplat64.dll')
  Write-Fake $savedHost 'MZ damaged'
  $damaged = Get-Snapshot
  Check ((Invoke-Probe 'probe_restore.ps1' @()) -eq 1 -and (Test-Says 'missing or damaged') -and (Test-Unchanged $damaged)) 'restore refuses a damaged backup'
  Write-Fake $savedHost $releaseHost

  # The owner turned a probe on: the yaml the install added is edited, so the restore leaves it. A copy that stopped
  # partway left a partial file beside the host, which the restore removes.
  $yamlPath = [System.IO.Path]::Combine($root, 'maps_probe.yaml')
  [System.IO.File]::WriteAllText($yamlPath, ([System.IO.File]::ReadAllText($yamlPath) -replace 'rules: off', 'rules: on'))
  $partialHost = [System.IO.Path]::Combine($game, 'BugSplat64.dll.probe-partial')
  Write-Fake $partialHost 'MZ half a host'
  Check ((Invoke-Probe 'probe_restore.ps1' @()) -eq 0 -and (Test-Says 'leave    nevr folder\maps_probe.yaml') -and (Test-Says 'clean    game folder\BugSplat64.dll.probe-partial') -and (Test-NoPathsShown)) 'restore succeeds, leaves the edited yaml and removes the partial copy'
  Check (-not [System.IO.File]::Exists($partialHost)) 'the partial copy is gone'
  $restored = Get-Snapshot
  $expected = @(($b + 'game-BugSplat64.dll'), ($b + 'manifest.tsv'), ($b + 'restored.txt'), ($b + 'root-nevr-runtime.dll'), 'LocalAppData\nevr\echovr\maps_probe.yaml') | Sort-Object
  Check (((Compare-Snapshot $original $restored) -join '|') -eq ($expected -join '|')) 'restore puts back every replaced file; the config files never changed'
  Check ((Invoke-Probe 'probe_restore.ps1' @()) -eq 1 -and (Test-Says 'nothing to restore') -and (Test-Unchanged $restored)) 'a second restore finds nothing to restore'

  # Again with -Root: the edited yaml is now replaced, so it is backed up and put back. The host is put back by hand
  # first (an install that stopped partway): the restore takes it as already restored.
  $rootArgs = @('-Root', $root)
  Check ((Invoke-Probe 'probe_install.ps1' ($gameArgs + $rootArgs)) -eq 0 -and (Test-Says 'replace  nevr folder\maps_probe.yaml')) 'install with -Root replaces the existing yaml'
  $second = @(Get-BackupDirs | Where-Object { $_.Name -ne $backups[0].Name })
  Check ($second.Count -eq 1 -and [System.IO.File]::Exists([System.IO.Path]::Combine($second[0].FullName, 'root-maps_probe.yaml'))) 'the second backup holds the yaml'
  Write-Fake ([System.IO.Path]::Combine($game, 'BugSplat64.dll')) $releaseHost
  # And the runtime as an older install's in-place copy could leave it: half written, matching neither hash.
  Write-Fake $runtimePath 'MZ half a runtime'
  $half = Get-Snapshot
  Check ((Invoke-Probe 'probe_restore.ps1' $rootArgs) -eq 1 -and (Test-Says '-Force puts the backup back') -and (Test-Unchanged $half)) 'restore refuses a DLL that matches neither hash without -Force'
  Check ((Invoke-Probe 'probe_restore.ps1' ($rootArgs + '-Force')) -eq 0 -and (Test-Says 'ok       game folder\BugSplat64.dll') -and (Test-Says 'put back nevr folder\nevr-runtime.dll')) 'restore -Force succeeds over a half-done install'
  $after = Get-Snapshot
  $s = 'LocalAppData\nevr\echovr\installer\' + $second[0].Name + '\'
  $expected = @(($s + 'game-BugSplat64.dll'), ($s + 'manifest.tsv'), ($s + 'restored.txt'), ($s + 'root-maps_probe.yaml'), ($s + 'root-nevr-runtime.dll')) | Sort-Object
  Check (((Compare-Snapshot $restored $after) -join '|') -eq ($expected -join '|')) 'the second restore returns to the first restore''s state, edited yaml included'
} finally {
  Stop-FakeGame
  $env:LOCALAPPDATA = $realLocal
}
Check ((Get-RealBackupCount) -eq $realBackupsBefore) 'the real nevr folder got no probe backup'

if ($script:failures -gt 0) { Write-Output "$($script:failures) check(s) failed; scratch kept: $Scratch"; exit 1 }
Remove-Item -LiteralPath $Scratch -Recurse -Force
Write-Output 'all checks passed'
exit 0
