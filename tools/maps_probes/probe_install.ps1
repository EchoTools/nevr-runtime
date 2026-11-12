# Installs a nevr probe package (package_probes.ps1) over an existing nevr install, for the owner probes
# (OWNER_PROBES.md section 7). It replaces exactly three files, nevr-runtime.dll and maps_probe.yaml in the nevr folder and
# BugSplat64.dll in the game folder, after copying each one that exists into <nevr folder>\installer\probe-backup-<time>\
# with a manifest of SHA-256s. probe_restore.ps1 puts them back from that manifest. Each copy goes through a <file>.probe-partial
# beside it, swapped in once verified, so a run that stops partway leaves every file whole. Nothing else is written or deleted;
# config files are never read or written. The release install.ps1 cannot install these DLLs: it fetches the signed
# release ones over them.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File probe_install.ps1 [-Dir <...\bin\win10>] [-Root <nevr folder>] [-WhatIf]
param([string]$Dir = '', [string]$Root = '', [switch]$WhatIf)
. (Join-Path $PSScriptRoot 'probe_common.ps1')

$manifestWritten = $false
try {
  Assert-GameStopped

  # The package: its three files must match PROBE_SHA256SUMS, and both DLLs must be the dev build. A release host
  # refuses the unsigned probe runtime, and a release runtime could replace itself with an update.
  $package = $PSScriptRoot
  $sumsFile = [System.IO.Path]::Combine($package, 'PROBE_SHA256SUMS')
  if (-not [System.IO.File]::Exists($sumsFile)) { throw 'PROBE_SHA256SUMS is not next to probe_install.ps1' }
  $wanted = @{}
  foreach ($line in [System.IO.File]::ReadAllLines($sumsFile)) {
    if ($line -match '^([0-9a-f]{64}) [ *]?(\S+)$') { $wanted[$Matches[2]] = $Matches[1] }
  }
  foreach ($file in $ProbeFiles) {
    $source = [System.IO.Path]::Combine($package, $file.Name)
    if (-not $wanted.ContainsKey($file.Name)) { throw "PROBE_SHA256SUMS has no entry for $($file.Name)" }
    $got = Get-Sha $source
    if ($got -ne $wanted[$file.Name]) { throw "$($file.Name) in the package does not match PROBE_SHA256SUMS: the package is damaged" }
  }
  if (-not (Test-HasLiteral ([System.IO.Path]::Combine($package, 'BugSplat64.dll')) 'NEVR-HOST-BUILD dev')) {
    throw 'the package BugSplat64.dll is not a dev host; it would refuse the probe runtime. Ask for a new package.'
  }
  if (-not (Test-HasLiteral ([System.IO.Path]::Combine($package, 'nevr-runtime.dll')) 'NEVR-PLUGIN-TRUST dev')) {
    throw 'the package nevr-runtime.dll is not a dev build. Ask for a new package.'
  }

  $nevr = Resolve-NevrRoot $Root
  $nevrRoot = $nevr.Path
  if (-not (Test-NevrBinary ([System.IO.Path]::Combine($nevrRoot, 'nevr-runtime.dll')))) {
    throw "no nEVR runtime in $($nevr.Shown): this is not a nevr install. Install the nevr release with its install.ps1 first."
  }
  $gameDir = Find-GameDir $Dir
  if (-not (Test-NevrBinary ([System.IO.Path]::Combine($gameDir, 'BugSplat64.dll')))) {
    throw 'the game folder BugSplat64.dll is not a nEVR host: nevr is not installed in this game. Install the nevr release with its install.ps1 first.'
  }
  $open = @(Get-OpenBackups $nevrRoot)
  if ($open.Count -gt 0) {
    throw "a probe install is already in place (nevr folder\installer\$($open[0].Name)); run probe_restore.ps1 first. Nothing was changed."
  }

  $plan = @()
  foreach ($file in $ProbeFiles) {
    $target = [System.IO.Path]::Combine((Get-PlaceDir $file.Place $nevrRoot $gameDir), $file.Name)
    $plan += @{ File = $file; Target = $target; Before = (Get-Sha $target); Installed = $wanted[$file.Name]
      Source = [System.IO.Path]::Combine($package, $file.Name); Backup = ($file.Place + '-' + $file.Name) }
  }

  $backupName = 'probe-backup-' + (Get-Date).ToString('yyyyMMdd-HHmmss')
  $backupDir = [System.IO.Path]::Combine($nevrRoot, 'installer', $backupName)
  Write-Output "nevr folder: $($nevr.Shown)"
  foreach ($step in $plan) {
    $shown = Get-ShownName $step.File.Place $step.File.Name
    if ($step.Before -eq 'absent') { Write-Output "  add      $shown (none there now)" }
    else { Write-Output "  replace  $shown (backed up first)" }
  }
  Write-Output "  backup   nevr folder\installer\$backupName"
  if ($WhatIf) {
    Write-Output '-WhatIf: nothing was written.'
    exit 0
  }

  if ([System.IO.Directory]::Exists($backupDir)) { throw "nevr folder\installer\$backupName already exists; run again in a second" }
  [System.IO.Directory]::CreateDirectory($backupDir) | Out-Null
  $lines = @($ManifestHeader, ("game_dir`t" + $gameDir))
  foreach ($step in $plan) {
    if ($step.Before -ne 'absent') {
      Copy-Verified $step.Target ([System.IO.Path]::Combine($backupDir, $step.Backup)) $step.Before
    }
    $lines += ("file`t{0}`t{1}`t{2}`t{3}" -f $step.File.Place, $step.File.Name, $step.Before, $step.Installed)
  }
  # The manifest goes in whole before any target is written, so a run that stops partway can be restored.
  $manifest = [System.IO.Path]::Combine($backupDir, 'manifest.tsv')
  [System.IO.File]::WriteAllLines($manifest + '.tmp', [string[]]$lines, (New-Object System.Text.UTF8Encoding $false))
  [System.IO.File]::Move($manifest + '.tmp', $manifest)
  $manifestWritten = $true

  foreach ($step in $plan) {
    Copy-Verified $step.Source $step.Target $step.Installed
    Write-Output ('installed ' + (Get-ShownName $step.File.Place $step.File.Name))
  }
  Write-Output "Probe build installed. Backup and manifest: nevr folder\installer\$backupName"
  Write-Output 'To undo: powershell -NoProfile -ExecutionPolicy Bypass -File probe_restore.ps1, with the same -Root if you gave one'
  exit 0
} catch {
  Write-Output ('ERROR: ' + $_.Exception.Message)
  if ($manifestWritten) { Write-Output 'Some files may already be replaced: run probe_restore.ps1 to put back what was there.' }
  exit 1
}
