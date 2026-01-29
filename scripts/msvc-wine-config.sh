#!/bin/bash
# MSVC Wine Configuration
# Copy this file to msvc-wine-config.sh and customize the paths for your setup.

# ============================================================================
# Unix filesystem paths (used by setup-msvc-wine.sh)
# ============================================================================

# Root directory containing VS2022 and Windows Kits folders
VS_SDK_ROOT="$HOME/opt"

# Visual Studio installation path (Unix path)
VS_ROOT="$VS_SDK_ROOT/vs2022-community"

# MSVC version (folder name under VC/Tools/MSVC/)
MSVC_VERSION="14.50.35717"

# Windows SDK version
SDK_VERSION="10.0.26100.0"

# ============================================================================
# Wine paths (used by cl-wine.sh, link-wine.sh, lib-wine.sh)
# These should match where setup-msvc-wine.sh maps the drive
# ============================================================================

# Wine drive letter mapped to VS_SDK_ROOT (default: w:)
WINE_DRIVE="w:"

# MSVC installation path (Wine path)
MSVC_ROOT="${WINE_DRIVE}\\vs2022-community\\VC\\Tools\\MSVC\\${MSVC_VERSION}"

# Windows SDK installation path (Wine path)
SDK_ROOT="${WINE_DRIVE}\\Windows Kits\\10"

# ============================================================================
# Wine configuration (optional)
# ============================================================================

# Wine prefix for MSVC (optional, defaults to $HOME/.wine-msvc)
# WINEPREFIX="$HOME/.wine-msvc"

# ============================================================================
# Export for CMake toolchain (optional)
# Uncomment these to have cmake pick up the values automatically
# ============================================================================

# export VS_SDK_ROOT
# export MSVC_VERSION
# export SDK_VERSION
