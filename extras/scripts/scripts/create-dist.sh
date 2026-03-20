#!/bin/bash
# Creates distribution packages for nevr-server
# Usage: ./scripts/create-dist.sh [--lite] [build_dir]
#
# Options:
#   --lite    Create stripped distribution without debug symbols
#
# This script creates tar.zst and .zip distribution packages containing:
# - All DLL files (GamePatches, GamePatchesLegacy, GameServer, GameServerLegacy, TelemetryAgent)
# - Debug symbol files (.dbg) - only in full distribution
# - NEVRSupervisor.ps1 script
# - README.md

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Parse arguments
LITE_MODE=0
BUILD_DIR=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --lite)
            LITE_MODE=1
            shift
            ;;
        *)
            BUILD_DIR="$1"
            shift
            ;;
    esac
done

BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build/linux-wine-linux-wine-release}"

# Get version from git
VERSION=$(git -C "$PROJECT_DIR" describe --tags --abbrev=4 --long --match "v*" 2>/dev/null | sed 's/^v//' | sed 's/-\([0-9]*\)-g\([a-f0-9]*\)$/+\1.\2/' || echo "0.0.0")

# If version still has issues, fallback
if [[ -z "$VERSION" ]] || [[ "$VERSION" == "0.0.0" ]]; then
    COMMIT=$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo "unknown")
    VERSION="0.0.0+${COMMIT}"
fi

# Set distribution name based on mode
if [[ $LITE_MODE -eq 1 ]]; then
    DIST_NAME="nevr-server-v${VERSION}-lite"
    echo "========================================"
    echo "Creating LITE distribution: $DIST_NAME"
    echo "(stripped DLLs, no debug symbols)"
else
    DIST_NAME="nevr-server-v${VERSION}"
    echo "========================================"
    echo "Creating distribution: $DIST_NAME"
fi
echo "Build directory: $BUILD_DIR"
echo "========================================"

DIST_DIR="$BUILD_DIR/dist"
DIST_PACKAGE_DIR="$DIST_DIR/$DIST_NAME"

# Verify build artifacts exist
BIN_DIR="$BUILD_DIR/bin"
REQUIRED_DLLS=(
    "GamePatches.dll"
    "GamePatchesLegacy.dll"
    "GameServer.dll"
    "GameServerLegacy.dll"
    "TelemetryAgent.dll"
)

REQUIRED_DBG=(
    "GamePatches.dll.dbg"
    "GamePatchesLegacy.dll.dbg"
    "GameServer.dll.dbg"
    "GameServerLegacy.dll.dbg"
    "TelemetryAgent.dll.dbg"
)

echo ""
echo "Checking for required artifacts..."

MISSING=0
for dll in "${REQUIRED_DLLS[@]}"; do
    if [[ ! -f "$BIN_DIR/$dll" ]]; then
        echo "  ERROR: Missing $BIN_DIR/$dll"
        MISSING=1
    else
        echo "  Found: $dll"
    fi
done

for dbg in "${REQUIRED_DBG[@]}"; do
    if [[ ! -f "$BUILD_DIR/$dbg" ]]; then
        if [[ $LITE_MODE -eq 0 ]]; then
            echo "  WARNING: Missing debug symbols $dbg"
        fi
    else
        if [[ $LITE_MODE -eq 0 ]]; then
            echo "  Found: $dbg"
        else
            echo "  Skipping: $dbg (lite mode)"
        fi
    fi
done

if [[ ! -f "$BUILD_DIR/NEVRSupervisor.ps1" ]]; then
    echo "  WARNING: Missing NEVRSupervisor.ps1"
else
    echo "  Found: NEVRSupervisor.ps1"
fi

if [[ $MISSING -eq 1 ]]; then
    echo ""
    echo "ERROR: Missing required DLLs. Please build the project first:"
    echo "  cmake --build $BUILD_DIR"
    exit 1
fi

# Clean and create distribution directory
echo ""
echo "Preparing distribution directory..."
rm -rf "$DIST_DIR"
mkdir -p "$DIST_PACKAGE_DIR"

# Copy DLLs
echo "Copying DLLs..."
for dll in "${REQUIRED_DLLS[@]}"; do
    cp "$BIN_DIR/$dll" "$DIST_PACKAGE_DIR/"
done

# Strip DLLs if in lite mode
if [[ $LITE_MODE -eq 1 ]]; then
    echo "Stripping DLLs..."
    STRIP_TOOL=$(command -v llvm-strip 2>/dev/null || command -v strip 2>/dev/null)
    if [[ -z "$STRIP_TOOL" ]]; then
        echo "  WARNING: No strip tool found, DLLs will not be stripped"
    else
        for dll in "${REQUIRED_DLLS[@]}"; do
            echo "  Stripping $dll..."
            "$STRIP_TOOL" --strip-all "$DIST_PACKAGE_DIR/$dll" 2>/dev/null || \
            "$STRIP_TOOL" -s "$DIST_PACKAGE_DIR/$dll" 2>/dev/null || \
            echo "  WARNING: Failed to strip $dll"
        done
    fi
fi

# Copy debug symbols (if they exist and not in lite mode)
if [[ $LITE_MODE -eq 0 ]]; then
    echo "Copying debug symbols..."
    for dbg in "${REQUIRED_DBG[@]}"; do
        if [[ -f "$BUILD_DIR/$dbg" ]]; then
            cp "$BUILD_DIR/$dbg" "$DIST_PACKAGE_DIR/"
        fi
    done
else
    echo "Skipping debug symbols (lite mode)"
fi

# Copy supervisor script (if exists)
if [[ -f "$BUILD_DIR/NEVRSupervisor.ps1" ]]; then
    echo "Copying NEVRSupervisor.ps1..."
    cp "$BUILD_DIR/NEVRSupervisor.ps1" "$DIST_PACKAGE_DIR/"
fi

# Copy README
if [[ -f "$PROJECT_DIR/README.md" ]]; then
    echo "Copying README.md..."
    cp "$PROJECT_DIR/README.md" "$DIST_PACKAGE_DIR/"
fi

# Create tar.zst archive
echo ""
echo "Creating $DIST_NAME.tar.zst..."
cd "$DIST_DIR"
tar --zstd -cvf "${DIST_NAME}.tar.zst" "$DIST_NAME"

# Create zip archive
echo ""
echo "Creating $DIST_NAME.zip..."
cd "$DIST_DIR"
zip -r "${DIST_NAME}.zip" "$DIST_NAME"

# Print summary
echo ""
echo "========================================"
echo "Distribution packages created:"
echo "  $DIST_DIR/${DIST_NAME}.tar.zst"
echo "  $DIST_DIR/${DIST_NAME}.zip"
echo ""
echo "Contents:"
ls -lh "$DIST_PACKAGE_DIR/"
echo ""
echo "Archive sizes:"
ls -lh "$DIST_DIR/${DIST_NAME}".{tar.zst,zip}
echo "========================================"
