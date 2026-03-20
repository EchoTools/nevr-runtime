#!/bin/bash
# Wrapper script for running protoc.exe via Wine
# Converts Unix paths to Wine paths

export WINEPREFIX="$HOME/.wine-msvc"
export WINEDEBUG="-all"
export WINEARCH="win64"
# Disable GUI windows (winedbg, crash dialogs, etc.)
export DISPLAY=""
export WINEDLLOVERRIDES="winedbg.exe=d"

# Function to convert a Unix path to Wine path
convert_path() {
    local path="$1"
    if [[ "$path" == /* ]]; then
        echo "z:${path//\//\\}"
    else
        echo "$path"
    fi
}

PROTOC_EXE="$1"
shift

# Convert paths in arguments
args=()
for arg in "$@"; do
    case "$arg" in
        --cpp_out=*|--proto_path=*|-I/*)
            # Handle options with paths
            if [[ "$arg" == -I/* ]]; then
                # Handle -I/path format
                prefix="-I"
                path="${arg:2}"
                args+=("${prefix}$(convert_path "$path")")
            else
                # Handle --option=/path format
                opt="${arg%%=*}"
                path="${arg#*=}"
                if [[ "$path" == /* ]]; then
                    args+=("${opt}=$(convert_path "$path")")
                else
                    args+=("$arg")
                fi
            fi
            ;;
        -I)
            # -I followed by path as separate arg
            args+=("$arg")
            ;;
        /*)
            # Absolute paths
            args+=("$(convert_path "$arg")")
            ;;
        *)
            args+=("$arg")
            ;;
    esac
done

# Run protoc via Wine
exec wine "$(convert_path "$PROTOC_EXE")" "${args[@]}"
