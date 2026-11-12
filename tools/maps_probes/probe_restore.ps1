# Undoes probe_install.ps1 from its manifest (<nevr folder>\installer\probe-backup-<time>\manifest.tsv). Every check
# comes first and a refusal changes nothing. Each backup copy must match its recorded SHA-256, and each DLL must still be
# the probe file that was installed, or already the original. A file that was absent before the install is removed only
# while it is unchanged. An edited maps_probe.yaml is left in place, because release builds never read it. -Force also puts
# back (or removes) a DLL that matches neither hash, such as one an older probe_install.ps1 left half-copied. A
# <file>.probe-partial left by a copy that stopped is removed. The backup folder is kept and marked restored.txt. Nothing
# else is written or deleted; config files are never read or written.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File probe_restore.ps1 [-Root <nevr folder>] [-Backup <probe-backup-name>] [-Force] [-WhatIf]
param([string]$Root = '', [string]$Backup = '', [switch]$Force, [switch]$WhatIf)
. (Join-Path $PSScriptRoot 'probe_common.ps1')

try {
  Assert-GameStopped
  $nevr = Resolve-NevrRoot $Root
  $nevrRoot = $nevr.Path

  $open = @(Get-OpenBackups $nevrRoot)
  if ($Backup -ne '') {
    $open = @($open | Where-Object { $_.Name -ieq $Backup })
    if ($open.Count -eq 0) { throw "no unrestored probe backup named $Backup in $($nevr.Shown)\installer" }
  }
  if ($open.Count -eq 0) { throw "no unrestored probe backup in $($nevr.Shown)\installer: nothing to restore" }
  if ($open.Count -gt 1) { throw ('more than one unrestored probe backup; pass -Backup with one of: ' + (($open | ForEach-Object { $_.Name }) -join ', ')) }
  $backupDir = $open[0].FullName
  $backupName = $open[0].Name

  $lines = [System.IO.File]::ReadAllLines([System.IO.Path]::Combine($backupDir, 'manifest.tsv'))
  if ($lines.Count -lt 2 -or $lines[0] -ne $ManifestHeader) { throw "$backupName\manifest.tsv is not a probe install manifest" }
  if ($lines[1] -notmatch "^game_dir`t(.+)$") { throw "$backupName\manifest.tsv has no game_dir" }
  $gameDir = $Matches[1]
  if (-not [System.IO.File]::Exists([System.IO.Path]::Combine($gameDir, 'echovr.exe'))) {
    throw 'the game folder the probe install used has no echovr.exe any more. Nothing was changed.'
  }

  # The manifest can name only the three probe files, each at its own place.
  $known = @{}
  foreach ($file in $ProbeFiles) { $known[$file.Place + '/' + $file.Name] = $file }
  $plan = @()
  $refusals = @()
  foreach ($line in @($lines | Select-Object -Skip 2)) {
    if ($line -eq '') { continue }
    $fields = $line.Split("`t")
    if ($fields.Count -ne 5 -or $fields[0] -ne 'file' -or -not $known.ContainsKey($fields[1] + '/' + $fields[2])) {
      throw "$backupName\manifest.tsv has a line this script does not know; nothing was changed"
    }
    $file = $known[$fields[1] + '/' + $fields[2]]
    $before = $fields[3]
    $installed = $fields[4]
    $shown = Get-ShownName $file.Place $file.Name
    $target = [System.IO.Path]::Combine((Get-PlaceDir $file.Place $nevrRoot $gameDir), $file.Name)
    $saved = [System.IO.Path]::Combine($backupDir, $file.Place + '-' + $file.Name)
    $current = Get-Sha $target
    $action = ''
    if ($before -eq 'absent') {
      if ($current -eq 'absent') { $action = 'none' }
      elseif ($current -eq $installed) { $action = 'remove' }
      elseif ($file.Mutable) { $action = 'leave' }
      elseif ($Force) { $action = 'remove' }
      else { $refusals += "$shown changed since the probe install (-Force removes it)" }
    } else {
      $savedSha = Get-Sha $saved
      if ($savedSha -ne $before) { $refusals += "the backup of $shown is missing or damaged ($backupName)" }
      elseif ($current -eq $before) { $action = 'none' }
      elseif ($current -eq $installed -or $file.Mutable -or $Force) { $action = 'put back' }
      else { $refusals += "$shown changed since the probe install (another installer ran, or a copy stopped partway? -Force puts the backup back)" }
    }
    $partial = $target + $PartialSuffix
    $plan += @{ Shown = $shown; Target = $target; Saved = $saved; Before = $before; Action = $action
      Partial = $(if ([System.IO.File]::Exists($partial)) { $partial } else { '' }) }
  }
  if ($refusals.Count -gt 0) { throw (($refusals -join '; ') + '. Nothing was changed.') }

  Write-Output "nevr folder: $($nevr.Shown), backup $backupName"
  foreach ($step in $plan) {
    switch ($step.Action) {
      'put back' { Write-Output "  put back $($step.Shown)" }
      'remove' { Write-Output "  remove   $($step.Shown) (the probe install added it)" }
      'leave' { Write-Output "  leave    $($step.Shown) (added by the probe install and edited since; release builds ignore it)" }
      'none' { Write-Output "  ok       $($step.Shown) (already as before)" }
    }
    if ($step.Partial -ne '') { Write-Output "  clean    $($step.Shown)$PartialSuffix (left by a copy that stopped)" }
  }
  if ($WhatIf) {
    Write-Output '-WhatIf: nothing was written.'
    exit 0
  }

  foreach ($step in $plan) {
    if ($step.Partial -ne '') { [System.IO.File]::Delete($step.Partial) }
    if ($step.Action -eq 'put back') { Copy-Verified $step.Saved $step.Target $step.Before }
    elseif ($step.Action -eq 'remove') { [System.IO.File]::Delete($step.Target) }
  }
  [System.IO.File]::WriteAllText([System.IO.Path]::Combine($backupDir, $RestoredMarker), (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + "`r`n")
  Write-Output "Restored. The backup stays in nevr folder\installer\$backupName."
  exit 0
} catch {
  Write-Output ('ERROR: ' + $_.Exception.Message)
  exit 1
}
