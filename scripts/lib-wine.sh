#!/bin/bash
# Wrapper script for MSVC lib.exe via Wine

export WINEPREFIX="$HOME/.wine-msvc"
export WINEDEBUG="-all"
export WINEARCH="win64"

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

# Function to check if a string looks like a Unix absolute path
is_unix_path() {
    local path="$1"
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
        /OUT:/*|/out:/*)
            prefix="${arg%%:*}:"
            path="${arg#*:}"
            if is_unix_path "$path"; then
                args+=("${prefix}$(convert_path "$path")")
            else
                args+=("$arg")
            fi
            ;;
        # Handle actual Unix paths as object files
        /home/*|/tmp/*|/usr/*|/var/*|/mnt/*|/opt/*|/etc/*|/lib/*|/bin/*)
            args+=("$(convert_path "$arg")")
            ;;
        # All other /X flags pass through as-is
        /*)
            args+=("$arg")
            ;;
        *)
            args+=("$arg")
            ;;
    esac
done

exec wine "${MSVC_ROOT}\\bin\\Hostx64\\x64\\lib.exe" "${args[@]}" </dev/null
