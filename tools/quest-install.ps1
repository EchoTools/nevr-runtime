# Quest install: a front end over `install-nevr-runtime.exe quest install` (ADR 0023, installer/). The core checks the signed
# release manifest, downloads the APK and the game data it names, checks both, plans from what the headset holds,
# installs, and writes the [NEVR.INSTALL] log. This script holds no hash, URL or version of its own: it finds the core
# and SideQuest's adb, runs the core, and shows the result.
# Right-click this file and choose "Run with PowerShell", or run:
# powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\quest-install.ps1
# -CoreArgs passes more options to the core (for example --channel or --version).
# -YesUninstall lets the core uninstall a differently signed app (and delete its app data) when the headset refuses the update.
# The window waits for Enter before it closes so a right-click run can be read; -NoPause skips that.
param(
    [string]$Core,
    [string]$Adb,
    [string]$Version,
    [string]$Serial,
    [string[]]$CoreArgs = @(),
    [switch]$YesUninstall,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'

function Find-Adb {
    if ($Adb) {
        if (-not (Test-Path -LiteralPath $Adb -PathType Leaf)) { throw "adb not found: $Adb" }
        return (Resolve-Path -LiteralPath $Adb).ProviderPath
    }
    $onPath = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $looked = New-Object System.Collections.Generic.List[string]
    $found = New-Object System.Collections.Generic.List[string]
    foreach ($root in @($env:LOCALAPPDATA, $env:APPDATA)) {
        if (-not $root) { $looked.Add('(unset LOCALAPPDATA or APPDATA)'); continue }
        $looked.Add("$root (SideQuest folders, then platform-tools\adb.exe)")
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        # SilentlyContinue: one folder this user cannot read must not stop the search of the others.
        $folders = @(Get-ChildItem -LiteralPath $root -Directory -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match 'sidequest' })
        if ((Split-Path -Path $root -Leaf) -match 'sidequest') {
            $folders += Get-Item -LiteralPath $root
        }
        foreach ($folder in $folders) {
            $looked.Add("$($folder.FullName)\**\platform-tools\adb.exe")
            Get-ChildItem -LiteralPath $folder.FullName -File -Filter adb.exe -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.Directory.Name -ieq 'platform-tools' } |
                ForEach-Object { if (-not $found.Contains($_.FullName)) { $found.Add($_.FullName) } }
        }
    }
    if ($found.Count -eq 0) {
        throw "SideQuest adb.exe not found. Looked in:`n$($looked -join "`n")`nInstall SideQuest or pass -Adb <path>."
    }
    if ($found.Count -ne 1) { throw "Multiple SideQuest adb.exe files found: $($found -join ', '). Pass -Adb <path>." }
    return $found[0]
}

$exitCode = 1
try {
    if (-not $Core) { $Core = Join-Path $PSScriptRoot 'install-nevr-runtime.exe' }
    if (-not (Test-Path -LiteralPath $Core -PathType Leaf)) { throw "install-nevr-runtime.exe not found: $Core (it ships beside this script in the Windows package; pass -Core <path>)" }
    $Core = (Resolve-Path -LiteralPath $Core).ProviderPath
    $adbPath = Find-Adb
    Write-Host "Using adb: $adbPath"
    $env:PATH = (Split-Path -Parent $adbPath) + [System.IO.Path]::PathSeparator + $env:PATH

    $arguments = @('quest', 'install')
    if ($Version) { $arguments += @('--version', $Version) }
    if ($Serial) { $arguments += @('--serial', $Serial) }
    if ($YesUninstall) { $arguments += '--yes-uninstall' }
    $arguments += $CoreArgs
    & $Core @arguments
    $exitCode = $LASTEXITCODE
} catch {
    [Console]::Error.WriteLine("error: $($_.Exception.Message)")
}
if (-not $NoPause) { Read-Host 'Press Enter to close' | Out-Null }
exit $exitCode
