# Dot-sourced by probe_install.ps1 and probe_restore.ps1. A probe install writes three files and nothing else:
# nevr-runtime.dll and maps_probe.yaml in the nevr folder, BugSplat64.dll (the host) in the game folder. The probe
# runtime is unsigned, so it needs the probe package's dev host: a release host refuses it and the game stops (ADR 0012).
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$ProbeFiles = @(
  @{ Name = 'nevr-runtime.dll'; Place = 'root'; Mutable = $false },
  @{ Name = 'BugSplat64.dll'; Place = 'game'; Mutable = $false },
  # The owner edits this one to turn probes on, so a changed copy is expected at restore.
  @{ Name = 'maps_probe.yaml'; Place = 'root'; Mutable = $true }
)
$ManifestHeader = 'nevr-probe-install 1'
$RestoredMarker = 'restored.txt'
$PartialSuffix = '.probe-partial'

function Get-Sha([string]$Path) {
  if (-not [System.IO.File]::Exists($Path)) { return 'absent' }
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower()
}

function Test-HasLiteral([string]$Path, [string]$Text) {
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  return [System.Text.Encoding]::GetEncoding(28591).GetString($bytes).Contains($Text)
}

# The literals install.ps1/uninstall.ps1 and package_release.py read: every nEVR runtime and host carries one of them.
function Test-NevrBinary([string]$Path) {
  if (-not [System.IO.File]::Exists($Path)) { return $false }
  $text = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($Path))
  return $text.Contains('NEVR-BUILD ') -or $text.Contains('nevr-runtime/1.0') -or $text.Contains('NEVR-HOST-BUILD ')
}

function Assert-GameStopped {
  if (@(Get-Process -Name echovr -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'echovr.exe is running; close the game (and any local server) first. Nothing was changed.'
  }
}

# The nevr folder as nevr's installer picks it: -Root (absolute), else %LOCALAPPDATA%\nevr\echovr.
function Resolve-NevrRoot([string]$Root) {
  if ($Root -ne '') {
    if (-not [System.IO.Path]::IsPathRooted($Root)) { throw '-Root must be an absolute path' }
    return @{ Path = [System.IO.Path]::GetFullPath($Root); Shown = 'the -Root folder' }
  }
  if (-not $env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set; pass -Root <nevr folder>' }
  return @{ Path = [System.IO.Path]::Combine($env:LOCALAPPDATA, 'nevr', 'echovr'); Shown = '%LOCALAPPDATA%\nevr\echovr' }
}

# The game folder as install.ps1 finds it: -Dir, else the usual Meta Horizon / Oculus library locations.
function Find-GameDir([string]$Dir) {
  if ($Dir -ne '') {
    if (-not [System.IO.Path]::IsPathRooted($Dir)) { throw '-Dir must be an absolute path' }
    $Dir = [System.IO.Path]::GetFullPath($Dir)
    if (-not [System.IO.File]::Exists([System.IO.Path]::Combine($Dir, 'echovr.exe'))) { throw 'no echovr.exe in the -Dir folder' }
    return $Dir
  }
  $libraryRoots = @('C:\Program Files\Meta Horizon', 'C:\Program Files\Oculus', 'C:\Program Files (x86)\Oculus',
    'D:\Oculus', 'D:\Meta Horizon', 'E:\Oculus', 'E:\Meta Horizon')
  try {
    $key = 'HKCU:\Software\Oculus VR, LLC\Oculus\Libraries'
    if (Test-Path -LiteralPath $key) {
      foreach ($lib in Get-ChildItem -LiteralPath $key) {
        foreach ($name in 'Path', 'OriginalPath') {
          $value = $lib.GetValue($name)
          if ($value) { $libraryRoots += ([string]$value) -replace '^\\\\\?\\', '' }
        }
      }
    }
  } catch {
    Write-Warning ('the Oculus library list could not be read: ' + $_.Exception.Message)
  }
  $suffixes = @('Software\Software\ready-at-dawn-echo-arena\bin\win10', 'Software\ready-at-dawn-echo-arena\bin\win10',
    'ready-at-dawn-echo-arena\bin\win10')
  $found = @()
  foreach ($libraryRoot in $libraryRoots) {
    foreach ($suffix in $suffixes) {
      $location = $libraryRoot.TrimEnd('\', '/') + '\' + $suffix
      if ([System.IO.File]::Exists($location + '\echovr.exe') -and ($found -inotcontains $location)) { $found += $location }
    }
  }
  if ($found.Count -gt 1) { throw ('more than one Echo VR install found; pass -Dir with one of: ' + ($found -join '; ')) }
  if ($found.Count -eq 0) { throw 'Echo VR was not found in the usual places; pass -Dir <...\ready-at-dawn-echo-arena\bin\win10>' }
  return $found[0]
}

function Get-PlaceDir([string]$Place, [string]$NevrRoot, [string]$GameDir) {
  if ($Place -eq 'root') { return $NevrRoot }
  if ($Place -eq 'game') { return $GameDir }
  throw "unknown place '$Place'"
}

function Get-ShownName([string]$Place, [string]$Name) {
  if ($Place -eq 'root') { return 'nevr folder\' + $Name }
  return 'game folder\' + $Name
}

# Copies to <To>.probe-partial and swaps it in only once it reads back right, so a copy that stops partway leaves <To> as it
# was, plus a partial file that probe_restore.ps1 removes.
function Copy-Verified([string]$From, [string]$To, [string]$Sha) {
  $partial = $To + $PartialSuffix
  [System.IO.File]::Copy($From, $partial, $true)
  $got = Get-Sha $partial
  if ($got -ne $Sha) {
    [System.IO.File]::Delete($partial)
    throw "the copy to $(Split-Path -Leaf $To) reads back as $got, not $Sha"
  }
  if ([System.IO.File]::Exists($To)) { [System.IO.File]::Replace($partial, $To, $null) }
  else { [System.IO.File]::Move($partial, $To) }
}

# Probe backups under <nevr folder>\installer that no restore has finished. One with no manifest replaced nothing.
function Get-OpenBackups([string]$NevrRoot) {
  $installerDir = [System.IO.Path]::Combine($NevrRoot, 'installer')
  if (-not [System.IO.Directory]::Exists($installerDir)) { return @() }
  return @(Get-ChildItem -LiteralPath $installerDir -Directory -Filter 'probe-backup-*' |
      Where-Object { [System.IO.File]::Exists([System.IO.Path]::Combine($_.FullName, 'manifest.tsv')) -and
        -not [System.IO.File]::Exists([System.IO.Path]::Combine($_.FullName, $RestoredMarker)) } |
      Sort-Object Name)
}
