# Packs a probe build into nevr-probes-<date>.zip. The build must be configured with --preset mingw-plugin-dev
# -DNEVR_MAPS_PROBES=ON. The zip holds nevr-runtime.dll, BugSplat64.dll, maps_probe.yaml, PROBE_SHA256SUMS and the owner's
# probe_install.ps1 / probe_restore.ps1 (with probe_common.ps1). The build must be the dev one: a release host refuses
# the unsigned probe runtime, and the game then stops (ADR 0012). The release install.ps1 is left out on purpose: its
# core installs package DLLs only when they match the signed manifest and downloads the release ones otherwise
# (installer/src/main.rs, --package-dir), so it would replace the probe DLLs.
#
# usage:  powershell -NoProfile -ExecutionPolicy Bypass -File tools\maps_probes\package_probes.ps1 -BuildDir <build dir> [-OutDir <dir>]
param([Parameter(Mandatory = $true)][string]$BuildDir, [string]$OutDir = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe_common.ps1')

if ($OutDir -eq '') { $OutDir = $BuildDir }

$cache = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cache)) { throw "no CMakeCache.txt in $BuildDir" }
foreach ($option in 'NEVR_MAPS_PROBES', 'NEVR_ALLOW_UNSIGNED_PLUGINS') {
  if (-not (Select-String -Path $cache -Pattern "^${option}:BOOL=ON$" -Quiet)) {
    throw "$BuildDir is not a probe build: $option is not ON (configure with --preset mingw-plugin-dev -DNEVR_MAPS_PROBES=ON)"
  }
}

function Find-One([string]$name) {
  $found = @(Get-ChildItem -Path $BuildDir -Recurse -Filter $name -File | Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' })
  if ($found.Count -ne 1) { throw "expected one $name under $BuildDir, found $($found.Count)" }
  return $found[0].FullName
}

$runtime = Find-One 'nevr-runtime.dll'
# Test shims (test_bootstrap_attach_host and others) are named BugSplat64.dll too; the host is nevr_host's, beside the runtime.
$hosts = @(Get-ChildItem -Path $BuildDir -Recurse -Filter 'BugSplat64.dll' -File | Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' })
if ($hosts.Count -gt 1) { $hosts = @($hosts | Where-Object { $_.DirectoryName -eq (Split-Path -Parent $runtime) }) }
if ($hosts.Count -ne 1) { throw "expected one nevr_host BugSplat64.dll under $BuildDir (beside nevr-runtime.dll), found $($hosts.Count)" }
$hostDll = $hosts[0].FullName
if (-not (Test-HasLiteral $hostDll 'NEVR-HOST-BUILD dev')) { throw 'BugSplat64.dll is not a dev host (NEVR-HOST-BUILD dev)' }
if (-not (Test-HasLiteral $runtime 'NEVR-PLUGIN-TRUST dev')) { throw 'nevr-runtime.dll is not a dev runtime (NEVR-PLUGIN-TRUST dev)' }

$stage = Join-Path $OutDir ('nevr-probes-' + (Get-Date -Format 'yyyyMMdd'))
if (Test-Path $stage) { Remove-Item -Recurse -Force -LiteralPath $stage }
$stage = (New-Item -ItemType Directory -Path $stage).FullName
Copy-Item $runtime $stage
Copy-Item $hostDll $stage
foreach ($name in 'maps_probe.yaml', 'probe_install.ps1', 'probe_restore.ps1', 'probe_common.ps1') {
  Copy-Item (Join-Path $PSScriptRoot $name) $stage
}
$sums = foreach ($file in $ProbeFiles) { (Get-Sha (Join-Path $stage $file.Name)) + '  ' + $file.Name }
[System.IO.File]::WriteAllLines((Join-Path $stage 'PROBE_SHA256SUMS'), [string[]]$sums, (New-Object System.Text.UTF8Encoding $false))

$zip = "$stage.zip"
if (Test-Path $zip) { Remove-Item -Force -LiteralPath $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Output "probe package: $zip"
