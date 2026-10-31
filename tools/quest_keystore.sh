#!/usr/bin/env bash
# The Quest APK signing key, copied into a private tmpfs file for exactly one command.
#
# Android only accepts an update whose signer matches the installed app's signer, so the key must
# be the same one on every build. It is never generated here. The commands that sign read two
# environment variables, set by the owner:
#
#   NEVR_QUEST_KEYSTORE       path to the keystore file (a JKS) that holds the one persistent key
#   NEVR_QUEST_KEYSTORE_PASS  its store passphrase (an environment variable, never an argument: `ps` shows arguments)
#
#   just quest-keystore-fingerprint           print the signer's SHA-256
#   tools/quest_keystore.sh check             exit 1 unless both variables name a keystore that opens
#   tools/quest_keystore.sh with <command...> run a command with NEVR_QUEST_KS set
#
# `with` exports, for the command's environment only:
#   NEVR_QUEST_KS        path to a mode-600 copy of the key, in a mode-700 directory on tmpfs
#   NEVR_QUEST_KS_ALIAS  the key's alias (nevr)
#   NEVR_QUEST_KS_PASS   the store passphrase, so apksigner/keytool read it with `env:` and it is
#                        never an argument (visible in `ps`)
# The copy is shredded and its directory removed on every exit path, including SIGTERM/SIGINT/SIGHUP.
set -euo pipefail

readonly ALIAS="nevr"
readonly KS_VAR="NEVR_QUEST_KEYSTORE"
readonly PASS_VAR="NEVR_QUEST_KEYSTORE_PASS"

# Set by materialize_key(); the EXIT trap cleans up whichever of them exists.
key=""
keydir=""
child=0

log() { printf '[quest-keystore] %s\n' "$*" >&2; }
err() { printf 'ERROR: %s\n' "$*" >&2; }

usage() {
    cat >&2 <<EOF
usage: $0 with <command...>
       $0 fingerprint
       $0 check

environment: $KS_VAR (path to the keystore file), $PASS_VAR (its passphrase)
EOF
    exit 2
}

cleanup() {
    local rc=$?
    # Run once: cleanup exits with the status it was called with.
    trap - EXIT
    if [[ -n "$key" && -e "$key" ]]; then
        # Overwrite the key bytes before unlinking; fall back to a plain remove if shred is unavailable.
        shred -u -- "$key" 2>/dev/null || rm -f -- "$key" 2>/dev/null || true
    fi
    if [[ -n "$keydir" && -d "$keydir" ]]; then
        rm -rf -- "$keydir" 2>/dev/null || true
    fi
    exit "$rc"
}

handle_signal() {
    local code="$1"
    # Stop the command first so the script does not outlive its child; the EXIT trap then shreds.
    if [[ "$child" -ne 0 ]]; then
        kill -TERM "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
        child=0
    fi
    exit "$code"
}

trap cleanup EXIT
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM
trap 'handle_signal 129' HUP

# Stop with the fix when either input is missing or unreadable. Nothing here prints the passphrase.
require_inputs() {
    [[ -n "${NEVR_QUEST_KEYSTORE:-}" ]] \
        || { err "$KS_VAR is not set: set it to the path of the Quest signing keystore file (the one persistent key; it is never generated here)"; exit 1; }
    [[ -f "$NEVR_QUEST_KEYSTORE" && -r "$NEVR_QUEST_KEYSTORE" ]] \
        || { err "$KS_VAR=$NEVR_QUEST_KEYSTORE is not a readable file"; exit 1; }
    [[ -n "${NEVR_QUEST_KEYSTORE_PASS:-}" ]] \
        || { err "$PASS_VAR is not set: set it to the keystore's passphrase (it is read from the environment, never from an argument)"; exit 1; }
}

# Open the keystore at $1 with the passphrase; a wrong passphrase or a missing alias stops here.
open_check() {
    command -v keytool >/dev/null 2>&1 || { err "keytool is required to validate the keystore"; exit 1; }
    if ! NEVR_QUEST_KS_PASS="$NEVR_QUEST_KEYSTORE_PASS" keytool -list -keystore "$1" -alias "$ALIAS" -storepass:env NEVR_QUEST_KS_PASS >/dev/null 2>&1; then
        err "$KS_VAR cannot be opened with $PASS_VAR (wrong passphrase, or no alias $ALIAS)"
        exit 1
    fi
}

# Copy the keystore into a private file and export the three NEVR_QUEST_KS* variables.
# Every failure exits 1 before the caller's command runs.
materialize_key() {
    local base
    require_inputs

    if [[ -n "${XDG_RUNTIME_DIR:-}" && -d "${XDG_RUNTIME_DIR:-}" && -w "${XDG_RUNTIME_DIR:-}" ]]; then
        base="$XDG_RUNTIME_DIR"
    else
        base="/dev/shm"
    fi
    if ! keydir="$(mktemp -d "$base/nevr-quest-keystore.XXXXXXXX" 2>/dev/null)"; then
        if [[ "$base" != "/dev/shm" ]]; then
            base="/dev/shm"
            keydir="$(mktemp -d "$base/nevr-quest-keystore.XXXXXXXX")" \
                || { err "cannot create a private temporary directory under $base"; exit 1; }
        else
            err "cannot create a private temporary directory under $base"
            exit 1
        fi
    fi
    chmod 700 "$keydir"
    key="$keydir/quest-signing-keystore.jks"

    umask 077
    cp -- "$NEVR_QUEST_KEYSTORE" "$key" || { err "cannot copy $KS_VAR into a private file"; exit 1; }
    chmod 600 "$key"
    # Check the copy, the file the signing command will read.
    open_check "$key"

    export NEVR_QUEST_KS="$key"
    export NEVR_QUEST_KS_ALIAS="$ALIAS"
    export NEVR_QUEST_KS_PASS="$NEVR_QUEST_KEYSTORE_PASS"
    log "keystore ready alias=$ALIAS"
}

# Print the signer's SHA-256 as lowercase hex with no colons, and nothing else on stdout.
signer_sha256() {
    local keystore="$1"
    keytool -list -keystore "$keystore" -alias "$ALIAS" -storepass:env NEVR_QUEST_KS_PASS 2>/dev/null \
        | awk -F': ' '/SHA-?256/ { print $2; exit }' \
        | tr -d ':[:space:]' \
        | tr 'A-F' 'a-f'
}

cmd_with() {
    [[ "$#" -ge 1 ]] || usage
    materialize_key

    "$@" &
    child=$!
    local status
    if wait "$child"; then
        status=0
    else
        status=$?
    fi
    child=0
    log "command exit=$status"
    exit "$status"
}

cmd_check() {
    require_inputs
    open_check "$NEVR_QUEST_KEYSTORE"
    log "keystore opens alias=$ALIAS"
}

cmd_fingerprint() {
    local fp
    materialize_key
    fp="$(signer_sha256 "$key")" || { err "could not read a SHA-256 fingerprint from the keystore"; exit 1; }
    [[ -n "$fp" ]] || { err "could not read a SHA-256 fingerprint from the keystore"; exit 1; }
    printf '%s\n' "$fp"
}

main() {
    [[ "$#" -ge 1 ]] || usage
    case "$1" in
        with)
            shift
            cmd_with "$@"
            ;;
        fingerprint)
            [[ "$#" -eq 1 ]] || usage
            cmd_fingerprint
            ;;
        check)
            [[ "$#" -eq 1 ]] || usage
            cmd_check
            ;;
        -h|--help|help)
            usage
            ;;
        *)
            err "unknown command: $1"
            usage
            ;;
    esac
}

main "$@"
