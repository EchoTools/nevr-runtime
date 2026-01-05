#!/bin/bash
# Wrapper script for MSVC link.exe via Wine

export WINEPREFIX="$HOME/.wine-msvc"
export WINEDEBUG="-all"
export WINEARCH="win64"
# Disable GUI windows (winedbg, crash dialogs, etc.)
export DISPLAY=""
export WINEDLLOVERRIDES="winedbg.exe=d"

MSVC_ROOT='w:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207'
SDK_ROOT='w:\Program Files (x86)\Windows Kits\10'
SDK_VERSION="10.0.26100.0"

# Set lib paths
export LIB="${MSVC_ROOT}\\lib\\x64;${SDK_ROOT}\\Lib\\${SDK_VERSION}\\ucrt\\x64;${SDK_ROOT}\\Lib\\${SDK_VERSION}\\um\\x64"

# Function to convert a Unix path to Wine path
convert_path() {
    local path="$1"
    if [[ "$path" == /* ]]; then
        echo "z:${path//\//\\}"
    else
        echo "$path"
    fi
}

# Convert paths in arguments
args=()
for arg in "$@"; do
    case "$arg" in
        /OUT:/*|/out:/*)
            prefix="${arg%%:*}:"
            path="${arg#*:}"
            args+=("${prefix}$(convert_path "$path")")
            ;;
        /LIBPATH:/*|/libpath:/*)
            prefix="${arg%%:*}:"
            path="${arg#*:}"
            args+=("${prefix}$(convert_path "$path")")
            ;;
        /IMPLIB:/*|/implib:/*)
            prefix="${arg%%:*}:"
            path="${arg#*:}"
            args+=("${prefix}$(convert_path "$path")")
            ;;
        /DEF:/*|/def:/*)
            prefix="${arg%%:*}:"
            path="${arg#*:}"
            args+=("${prefix}$(convert_path "$path")")
            ;;
        /PDB:/*|/pdb:/*)
            prefix="${arg%%:*}:"
            path="${arg#*:}"
            args+=("${prefix}$(convert_path "$path")")
            ;;
        # Handle object files and other absolute Unix paths
        /home/*|/tmp/*|/usr/*|/var/*|/mnt/*|/opt/*|/etc/*|/lib/*|/bin/*)
            args+=("$(convert_path "$arg")")
            ;;
        # Linker flags start with / but aren't Unix paths - keep as-is
        /*)
            args+=("$arg")
            ;;
        *)
            args+=("$arg")
            ;;
    esac
done

exec wine "${MSVC_ROOT}\\bin\\Hostx64\\x64\\link.exe" "${args[@]}" </dev/null
