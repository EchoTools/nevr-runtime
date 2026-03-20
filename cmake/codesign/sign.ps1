# Code sign Windows PE binaries using signtool or osslsigncode.
#
# Usage: sign.ps1 <file> [file ...]
#
# Environment variables:
#   CODESIGN_PFX       - Path to PKCS#12 bundle (.pfx/.p12)
#   CODESIGN_PASS      - Password for PFX file
#   CODESIGN_THUMBPRINT - Certificate thumbprint (for cert store signing)
#   CODESIGN_TSURL     - Timestamp server URL (optional)

param(
    [Parameter(Mandatory=$true, ValueFromRemainingArguments=$true)]
    [string[]]$Files
)

$ErrorActionPreference = "Stop"

# Locate signtool
$signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
if (-not $signtool) {
    # Try Windows SDK paths
    $sdkPaths = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue | Sort-Object -Descending
    if ($sdkPaths) {
        $signtool = $sdkPaths[0].FullName
    } else {
        Write-Error "signtool.exe not found. Install Windows SDK or add it to PATH."
        exit 1
    }
}

# Build signing arguments
$signArgs = @("sign", "/fd", "sha256", "/d", "NEVR Runtime")

if ($env:CODESIGN_TSURL) {
    $signArgs += "/tr", $env:CODESIGN_TSURL, "/td", "sha256"
}

if ($env:CODESIGN_PFX) {
    $signArgs += "/f", $env:CODESIGN_PFX
    if ($env:CODESIGN_PASS) {
        $signArgs += "/p", $env:CODESIGN_PASS
    }
} elseif ($env:CODESIGN_THUMBPRINT) {
    $signArgs += "/sha1", $env:CODESIGN_THUMBPRINT
} else {
    Write-Error "No signing credentials. Set CODESIGN_PFX or CODESIGN_THUMBPRINT."
    exit 1
}

foreach ($file in $Files) {
    if (-not (Test-Path $file)) {
        Write-Warning "$file not found, skipping"
        continue
    }
    Write-Host "Signing: $file"
    & $signtool ($signArgs + $file)
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to sign $file"
        exit 1
    }
}
