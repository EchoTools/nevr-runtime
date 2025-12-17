#!/bin/bash
# Wrapper script for MSVC cl.exe via Wine
# This script converts Unix paths to Wine paths and calls cl.exe

export WINEPREFIX="$HOME/.wine-msvc"
export WINEDEBUG="-all"
export WINEARCH="win64"

MSVC_ROOT='w:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207'
SDK_ROOT='w:\Program Files (x86)\Windows Kits\10'
SDK_VERSION="10.0.26100.0"

# Set include and lib paths for MSVC
export INCLUDE="${MSVC_ROOT}\\include;${SDK_ROOT}\\Include\\${SDK_VERSION}\\ucrt;${SDK_ROOT}\\Include\\${SDK_VERSION}\\um;${SDK_ROOT}\\Include\\${SDK_VERSION}\\shared"
export LIB="${MSVC_ROOT}\\lib\\x64;${SDK_ROOT}\\Lib\\${SDK_VERSION}\\ucrt\\x64;${SDK_ROOT}\\Lib\\${SDK_VERSION}\\um\\x64"

# Function to convert a Unix path to Wine path
convert_path() {
    local path="$1"
    if [[ "$path" == /* ]]; then
        # Convert absolute Unix path to Wine Z: drive path
        echo "z:${path//\//\\}"
    else
        echo "$path"
    fi
}

# Function to check if a string looks like a Unix absolute path
is_unix_path() {
    local path="$1"
    # Check if it starts with common Unix directories
    case "$path" in
        /home/*|/tmp/*|/usr/*|/var/*|/mnt/*|/opt/*|/etc/*|/lib/*|/bin/*|/proc/*|/sys/*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

# Convert paths in arguments
args=()
for arg in "$@"; do
    case "$arg" in
        # Handle -I include paths (with or without space)
        -I/*)
            prefix="-I"
            path="${arg:2}"
            if is_unix_path "/$path"; then
                args+=("${prefix}$(convert_path "$path")")
            else
                args+=("$arg")
            fi
            ;;
        # Handle /I include paths
        /I/*)
            prefix="/I"
            path="${arg:2}"
            if is_unix_path "/$path"; then
                args+=("${prefix}$(convert_path "$path")")
            else
                args+=("$arg")
            fi
            ;;
        # Handle MSVC options with paths: /Fo, /Fe, /Fd, /FI, /Fp, /Yc, /Yu
        # These take the path directly after the option letters
        /Fo/*|/Fe/*|/Fd/*|/FI/*|/Fp/*)
            prefix="${arg:0:3}"
            path="${arg:3}"
            if is_unix_path "$path"; then
                args+=("${prefix}$(convert_path "$path")")
            else
                args+=("$arg")
            fi
            ;;
        /Yc/*|/Yu/*)
            prefix="${arg:0:3}"
            path="${arg:3}"
            if is_unix_path "$path"; then
                args+=("${prefix}$(convert_path "$path")")
            else
                args+=("$arg")
            fi
            ;;
        # Handle source files and other absolute Unix paths
        /home/*|/tmp/*|/usr/*|/var/*|/mnt/*|/opt/*|/etc/*|/lib/*|/bin/*)
            args+=("$(convert_path "$arg")")
            ;;
        # All other /X options are MSVC flags - pass through as-is
        /*)
            args+=("$arg")
            ;;
        # Non-slash arguments pass through
        *)
            args+=("$arg")
            ;;
    esac
done

exec wine "${MSVC_ROOT}\\bin\\Hostx64\\x64\\cl.exe" "${args[@]}" </dev/null
