#!/bin/bash
# Build script for nevr-server using MSVC via Wine

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Wine configuration
export WINEPREFIX="$HOME/.wine-msvc"
export WINEDEBUG="-all"
export WINEARCH="win64"
# Disable GUI windows (winedbg, crash dialogs, etc.)
export DISPLAY=""
export WINEDLLOVERRIDES="winedbg.exe=d"

# Paths
WINOS="/mnt/winos"
MSVC_VER="14.44.35207"
SDK_VER="10.0.26100.0"

VS_PATH="$WINOS/Program Files/Microsoft Visual Studio/2022/Community"
MSVC_PATH="$VS_PATH/VC/Tools/MSVC/$MSVC_VER"
SDK_PATH="$WINOS/Program Files (x86)/Windows Kits/10"

# Wine path conversion functions
to_wine_path() {
    echo "z:${1//\//\\\\}"
}

# Build directory
BUILD_DIR="$PROJECT_DIR/build-linux"
mkdir -p "$BUILD_DIR"

echo "============================================"
echo "NEVR Server Build Script (MSVC via Wine)"
echo "============================================"
echo ""
echo "Project directory: $PROJECT_DIR"
echo "Build directory: $BUILD_DIR"
echo ""

# Setup environment variables for cl.exe
# Convert to Windows-style paths
WIN_MSVC_PATH='w:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207'
WIN_SDK_PATH='w:\Program Files (x86)\Windows Kits\10'

# Include paths (semicolon separated Windows paths)
export INCLUDE="${WIN_MSVC_PATH}\\include;${WIN_SDK_PATH}\\Include\\${SDK_VER}\\ucrt;${WIN_SDK_PATH}\\Include\\${SDK_VER}\\um;${WIN_SDK_PATH}\\Include\\${SDK_VER}\\shared"

# Library paths
export LIB="${WIN_MSVC_PATH}\\lib\\x64;${WIN_SDK_PATH}\\Lib\\${SDK_VER}\\ucrt\\x64;${WIN_SDK_PATH}\\Lib\\${SDK_VER}\\um\\x64"

# Function to run cl.exe
run_cl() {
    wine "${MSVC_PATH}/bin/Hostx64/x64/cl.exe" "$@"
}

# Function to run link.exe
run_link() {
    wine "${MSVC_PATH}/bin/Hostx64/x64/link.exe" "$@"
}

# Function to run lib.exe
run_lib() {
    wine "${MSVC_PATH}/bin/Hostx64/x64/lib.exe" "$@"
}

echo "Testing MSVC compiler..."
run_cl 2>&1 | head -3

echo ""
echo "Environment setup complete!"
echo ""
echo "To build manually, use:"
echo "  export WINEPREFIX=$WINEPREFIX"
echo "  export INCLUDE='$INCLUDE'"
echo "  export LIB='$LIB'"
echo "  wine '$MSVC_PATH/bin/Hostx64/x64/cl.exe' <args>"
