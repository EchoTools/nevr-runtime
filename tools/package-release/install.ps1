# NEVR runtime release: install.
#
# Puts this package's BugSplat64.dll into the Echo VR install and sets the old files aside. It never
# deletes anything: the original BugSplat64.dll is copied to BugSplat64.dll.original-<timestamp>, and a
# legacy dbgcore.dll is renamed to dbgcore.dll.legacy-<date>. uninstall.ps1 puts both back.
# Run again from a newer package, it updates the runtime in place and leaves those two set-aside files alone.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-Dir <...\ready-at-dawn-echo-arena\bin\win10>] [-Pause | -NoPause]
# Without -Dir the usual Meta Horizon / Oculus library locations are searched; with more than one
# install found, pass -Dir.
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
  foreach ($root in $roots) {
    foreach ($suffix in $suffixes) {
      $location = $root.TrimEnd('\', '/') + '\' + $suffix
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

$target = Join-Path $Dir 'BugSplat64.dll'
if (-not (Test-Path -LiteralPath $target)) { throw "no BugSplat64.dll in $Dir to back up" }
$state = Join-Path $Dir 'nevr-install.txt'
$originalSha = Get-Sha $target
# A NEVR runtime carries the NEVR-BUILD identity literal (#759).
function Test-NevrRuntime([string]$path) {
  return [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($path)).Contains('NEVR-BUILD ')
}

# #759: already installed means an update. The DLL must be one this folder's install put there (the recorded
# build or an earlier one); the original backup and the set-aside dbgcore.dll stay as the first install left
# them, so uninstall.ps1 still restores the oldest original. The new hash is recorded and the old ones kept.
if (Test-Path -LiteralPath $state) {
  $values = @{}
  foreach ($line in Get-Content -LiteralPath $state) {
    if ($line -match '^([a-z_0-9]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
  }
  foreach ($key in 'backup', 'legacy', 'installed_sha256') {
    if (-not $values.ContainsKey($key)) { throw "nevr-install.txt has no $key (see $state)" }
  }
  # #779: an install made before #762 may have saved a NEVR runtime as the original; never restore or keep one.
  if ((Test-Path -LiteralPath $values['backup']) -and (Test-NevrRuntime $values['backup'])) {
    throw "the backup $($values['backup']) is a NEVR runtime, not the game's own file (an earlier install.ps1 saved it); nothing was changed. Restore the game's original BugSplat64.dll (for example, Verify Files in the Meta Horizon app), delete $state, then run install.ps1 again"
  }
  $known = @($values['installed_sha256'])
  if ($values.ContainsKey('previous_sha256') -and $values['previous_sha256'] -ne '') { $known += $values['previous_sha256'].Split(',') }
  if ($originalSha -eq $ours) {
    if ($values['installed_sha256'] -eq $ours) { throw 'the installed BugSplat64.dll is already this package; nothing to do' }
    # This package's DLL was copied in by hand: record it so uninstall.ps1 and the next update know it.
    $previous = @($known | Where-Object { $_ -ne $ours } | Select-Object -Unique)
    Set-Content -LiteralPath $state -Encoding ASCII -Value @("backup=$($values['backup'])", "legacy=$($values['legacy'])", "installed_sha256=$ours", ('previous_sha256=' + ($previous -join ',')))
    Write-Output "RECORDED BugSplat64.dll sha256=$ours as installed in $Dir (it was already this package; the original backup is unchanged: $($values['backup']))"
    Write-RunLog ("OK $scriptName RECORDED BugSplat64.dll sha256=$ours Dir=$Dir")
    Wait-BeforeClose
    exit 0
  }
  if ($known -notcontains $originalSha) {
    if (Test-NevrRuntime $target) {
      throw "BugSplat64.dll is a NEVR runtime install.ps1 did not record; run uninstall.ps1 first (it accepts any NEVR runtime), then install.ps1 again (see $state)"
    }
    throw "BugSplat64.dll is neither one install.ps1 put there nor a NEVR runtime, so uninstall.ps1 cannot undo it either; nothing was changed. Restore the game's own file (for example, Verify Files in the Meta Horizon app), delete $state, then run install.ps1 again"
  }
  if (-not (Test-Path -LiteralPath $values['backup'])) { throw "the backup is gone: $($values['backup']); nothing was changed" }
  if (Test-Path -LiteralPath (Join-Path $Dir 'dbgcore.dll')) { throw 'a dbgcore.dll exists again; run uninstall.ps1 first' }
  $prior = [System.IO.File]::ReadAllBytes($target)
  try {
    Copy-Item -LiteralPath $dll -Destination $target -Force
    if ((Get-Sha $target) -ne $ours) { throw 'the installed file does not match the package' }
  } catch {
    Write-Output ("update failed: " + $_.Exception.Message + '; restoring')
    [System.IO.File]::WriteAllBytes($target, $prior)
    throw
  }
  $previous = @($known | Where-Object { $_ -ne $ours } | Select-Object -Unique)
  Set-Content -LiteralPath $state -Encoding ASCII -Value @("backup=$($values['backup'])", "legacy=$($values['legacy'])", "installed_sha256=$ours", ('previous_sha256=' + ($previous -join ',')))
  Write-Output "UPDATED BugSplat64.dll sha256=$ours into $Dir (the original backup is unchanged: $($values['backup']))"
  Write-RunLog ("OK $scriptName UPDATED BugSplat64.dll sha256=$ours Dir=$Dir")
  Wait-BeforeClose
  exit 0
}

if ($originalSha -eq $ours) { throw 'the installed BugSplat64.dll is already this package; nothing to do' }
# The file about to be backed up as the original must be the game's own. A NEVR runtime carries the
# NEVR-BUILD identity literal; backing one up would make uninstall.ps1 "restore" a runtime (#759).
if (Test-NevrRuntime $target) {
  throw 'the installed BugSplat64.dll is a NEVR runtime, not the game''s own file, and nevr-install.txt is missing; the file was left as it is. Restore the game''s original BugSplat64.dll first (for example, Verify Files in the Meta Horizon app), then run install.ps1 again'
}

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
Write-RunLog ("OK $scriptName INSTALLED BugSplat64.dll sha256=$ours Dir=$Dir")

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
