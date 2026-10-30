# NEVR runtime release: install.
#
# A front end over install-nevr-runtime.exe, the core installer that ships beside this script (ADR 0023). The core fetches
# the signed release manifest, checks it, downloads the Windows package the channel's release names, checks every
# file against the manifest, and then puts BugSplat64.dll (the host the game loads) into the Echo VR install and
# nevr-runtime.dll (the runtime the host loads) into the nevr folder (%LOCALAPPDATA%\nevr\echovr, ADR 0012). The only
# file written in the game folder is BugSplat64.dll. It deletes nothing: the original BugSplat64.dll is copied to
# <nevr folder>\installer\aside\BugSplat64.dll.original-<timestamp>, and a legacy dbgcore.dll, an earlier
# nevr-runtime.dll and an earlier nevr-install.txt found beside echovr.exe are moved there too. uninstall.ps1 puts the
# original back from <nevr folder>\installer\nevr-install.txt, which the core writes. Run again, it updates both files
# and leaves the set-aside files alone. Any failure puts every file back as it was. Every run writes a [NEVR.INSTALL]
# log; its path is in the output. This script holds no hash, URL or version of its own; it finds the game folder, runs
# the core and shows the result.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-Dir <...\ready-at-dawn-echo-arena\bin\win10>] [-Root <nevr folder>] [-Version <release>] [-AllowDowngrade] [-CoreArgs <extra install-nevr-runtime options>] [-Pause | -NoPause]
# Without -Dir the usual Meta Horizon / Oculus library locations are searched; with more than one
# install found, pass -Dir. -Root names the nevr folder; the core accepts only the folder the game loads nEVR from
# (%LOCALAPPDATA%\nevr\echovr, the default) and refuses any other (#1353). A dev or test run passes --dev-nevr-root in
# -CoreArgs. Without -Version the release the signed manifest names for the channel is installed; an older release over
# a newer one is refused unless -AllowDowngrade is given.
param([string]$Dir = '', [string]$Root = '', [string]$Version = '', [switch]$AllowDowngrade, [string[]]$CoreArgs = @(), [switch]$Pause, [switch]$NoPause)
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
# The core is the one thing this script runs. It sits beside this script in the package; SHA256SUMS in the package lists
# it, so a damaged download is named here, before the core is trusted to run.
$core = Join-Path $here 'install-nevr-runtime.exe'
if (-not (Test-Path -LiteralPath $core)) { throw "install-nevr-runtime.exe is not next to install.ps1 ($here)" }
$sums = Join-Path $here 'SHA256SUMS'
if (-not (Test-Path -LiteralPath $sums)) { throw "SHA256SUMS is not next to install.ps1 ($here)" }
$wanted = $null
foreach ($line in Get-Content -LiteralPath $sums) {
  if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$' -and $Matches[2] -ceq 'install-nevr-runtime.exe') { $wanted = $Matches[1].ToLower() }
}
if (-not $wanted) { throw 'SHA256SUMS has no entry for install-nevr-runtime.exe' }
$ours = Get-Sha $core
if ($ours -ne $wanted) { throw "install-nevr-runtime.exe does not match SHA256SUMS (got $ours): the package is damaged, download it again" }

# Join-Path throws DriveNotFoundException for a drive this PC does not have (#679), so a candidate
# folder is joined by plain concatenation and tested with File.Exists, which answers False instead.
function Get-ExePath([string]$Folder) { [System.IO.Path]::Combine($Folder, 'echovr.exe') }
function Test-Echo([string]$Folder) { [System.IO.File]::Exists((Get-ExePath $Folder)) }

# The library folders the Oculus / Meta Horizon app records for this user (value Path, or
# OriginalPath when the library was moved). A missing key or an unreadable one adds nothing.
# PowerShell names are case-insensitive: nothing below may be called $root, which is the -Root parameter.
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
  $libraryRoots = @('C:\Program Files\Meta Horizon', 'C:\Program Files\Oculus', 'C:\Program Files (x86)\Oculus',
             'D:\Oculus', 'D:\Meta Horizon', 'E:\Oculus', 'E:\Meta Horizon') + @(Get-LibraryRoots)
  $suffixes = @('Software\Software\ready-at-dawn-echo-arena\bin\win10', 'Software\ready-at-dawn-echo-arena\bin\win10',
                'ready-at-dawn-echo-arena\bin\win10')
  $found = @()
  foreach ($libraryRoot in $libraryRoots) {
    foreach ($suffix in $suffixes) {
      $location = $libraryRoot.TrimEnd('\', '/') + '\' + $suffix
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

# --package-dir: the core installs this package's own DLLs when their SHA-256 matches the signed manifest's entry for the
# release, and downloads the package the manifest names only when they are missing or differ.
$arguments = @('pc', 'install', '--dir', $Dir, '--package-dir', $here)
if ($Root -ne '') {
  if (-not [System.IO.Path]::IsPathRooted($Root)) { throw "-Root must be an absolute path (got $Root)" }
  $arguments += @('--nevr-root', $Root)
}
if ($AllowDowngrade) { $arguments += '--allow-downgrade' }
if ($Version) { $arguments += @('--version', $Version) }
$arguments += $CoreArgs
Write-RunLog ("RUN $scriptName install-nevr-runtime.exe $($arguments -join ' ')")
# The core prints one JSON object on stdout (state, reason, exit code, log path, plan) and "[state] reason" on stderr.
# stderr is not redirected: Windows PowerShell 5.1 turns redirected native stderr into a terminating error here.
$output = & $core @arguments
$exitCode = $LASTEXITCODE
foreach ($line in $output) { Write-Host $line }
Write-RunLog ("END $scriptName install-nevr-runtime.exe exit=$exitCode Dir=$Dir")
if ($exitCode -eq 0) {
  $undo = "powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 -Dir `"$Dir`""
  if ($Root -ne '') { $undo += " -Root `"$Root`"" }
  Write-Output "To undo: $undo"
}

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
