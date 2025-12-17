#!/bin/bash
# Setup script for using MSVC from Windows partition via Wine

set -e

# Paths to Windows installation
WINOS_ROOT="/mnt/winos"
VS_ROOT="$WINOS_ROOT/Program Files/Microsoft Visual Studio/2022/Community"
MSVC_ROOT="$VS_ROOT/VC/Tools/MSVC/14.44.35207"
SDK_ROOT="$WINOS_ROOT/Program Files (x86)/Windows Kits/10"
SDK_VERSION="10.0.26100.0"

# Wine prefix directory
export WINEPREFIX="$HOME/.wine-msvc"
export WINEARCH="win64"
export WINEDEBUG="-all"

echo "Setting up Wine prefix for MSVC..."
mkdir -p "$WINEPREFIX"

# Check if Wine prefix exists
if [ ! -d "$WINEPREFIX/drive_c" ]; then
    echo "Initializing Wine prefix..."
    wineboot --init
fi

# Create symlinks in Wine prefix to access Windows installation
echo "Creating symlinks to Windows tools..."
mkdir -p "$WINEPREFIX/dosdevices"
ln -sf "$WINOS_ROOT" "$WINEPREFIX/dosdevices/w:"

echo ""
echo "Wine prefix configured at: $WINEPREFIX"
echo "Windows drive mapped to: w:\\"
echo ""
echo "MSVC paths:"
echo "  - MSVC: w:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.44.35207"
echo "  - SDK:  w:\\Program Files (x86)\\Windows Kits\\10"
echo ""
echo "To use cl.exe:"
echo "  WINEPREFIX=$WINEPREFIX wine 'w:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x64\\cl.exe'"
