#!/usr/bin/env bash
# The Quest APK signing key, read from `pass` into a private tmpfs file for exactly one command.
#
# Android only accepts an update whose signer matches the installed app's signer, so the key must
# be the same one on every build. It is stored once in `pass` and never generated per build:
#
#   just import-quest-keystore <keystore>     store the key and its passphrase (once)
#   just quest-keystore-fingerprint           print the signer's SHA-256
#   tools/quest_keystore.sh with <command...> run a command with NEVR_QUEST_KS set
#
# `with` exports, for the command's environment only:
#   NEVR_QUEST_KS        path to a mode-600 copy of the key, in a mode-700 directory on tmpfs
#   NEVR_QUEST_KS_ALIAS  the key's alias (nevrdebug)
#   NEVR_QUEST_KS_PASS   the store passphrase, so apksigner/keytool read it with `env:` and it is
#                        never an argument (visible in `ps`)
# The copy is shredded and its directory removed on every exit path, including SIGTERM/SIGINT/SIGHUP.
set -euo pipefail

readonly KS_ENTRY="services/echovrce/quest-signing-keystore"
readonly PASS_ENTRY="services/echovrce/quest-signing-keystore-pass"
readonly ALIAS="nevrdebug"
readonly IMPORT_HINT="just import-quest-keystore <keystore>"

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
       $0 import <keystore-file>
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

# Materialize the stored keystore as a private file and export the three NEVR_QUEST_KS* variables.
# Every failure exits 1 with a message that names the import command; the caller's command never runs.
materialize_key() {
    local b64 passphrase base

    command -v pass >/dev/null 2>&1 || { err "pass is not installed"; exit 1; }

    if ! b64="$(pass show "$KS_ENTRY" 2>/dev/null)"; then
        err "no Quest signing keystore in pass at $KS_ENTRY; store one with: $IMPORT_HINT"
        exit 1
    fi
    if ! passphrase="$(pass show "$PASS_ENTRY" 2>/dev/null)"; then
        err "no Quest signing keystore passphrase in pass at $PASS_ENTRY; store one with: $IMPORT_HINT"
        exit 1
    fi
    [[ -n "$b64" ]] || { err "the stored Quest signing keystore is empty; re-import it with: $IMPORT_HINT"; exit 1; }
    [[ -n "$passphrase" ]] || { err "the stored Quest signing keystore passphrase is empty; re-import it with: $IMPORT_HINT"; exit 1; }

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
    if ! printf '%s' "$b64" | base64 -d > "$key"; then
        err "the stored Quest signing keystore is not valid base64; re-import it with: $IMPORT_HINT"
        exit 1
    fi
    chmod 600 "$key"
    [[ -s "$key" ]] || { err "the stored Quest signing keystore decoded to nothing; re-import it with: $IMPORT_HINT"; exit 1; }

    export NEVR_QUEST_KS="$key"
    export NEVR_QUEST_KS_ALIAS="$ALIAS"
    export NEVR_QUEST_KS_PASS="$passphrase"

    command -v keytool >/dev/null 2>&1 || { err "keytool is required to validate the keystore"; exit 1; }
    if ! keytool -list -keystore "$key" -alias "$ALIAS" -storepass:env NEVR_QUEST_KS_PASS >/dev/null 2>&1; then
        err "the stored keystore cannot be opened with the stored passphrase (or has no alias $ALIAS); re-import it with: $IMPORT_HINT"
        exit 1
    fi
    log "keystore ready entry=$KS_ENTRY alias=$ALIAS"
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

cmd_fingerprint() {
    local fp
    materialize_key
    fp="$(signer_sha256 "$key")" || { err "could not read a SHA-256 fingerprint from the stored keystore"; exit 1; }
    [[ -n "$fp" ]] || { err "could not read a SHA-256 fingerprint from the stored keystore"; exit 1; }
    printf '%s\n' "$fp"
}

cmd_import() {
    local file="${1:-}" passphrase fp b64

    [[ "$#" -eq 1 && -n "$file" ]] || usage
    [[ -f "$file" ]] || { err "keystore file not found: $file"; exit 1; }

    command -v pass >/dev/null 2>&1 || { err "pass is not installed"; exit 1; }
    command -v keytool >/dev/null 2>&1 || { err "keytool is required"; exit 1; }
    command -v base64 >/dev/null 2>&1 || { err "base64 is required"; exit 1; }

    # A different key forces an uninstall on the headset, so never replace one silently.
    if pass show "$KS_ENTRY" >/dev/null 2>&1; then
        err "a Quest signing keystore is already stored at $KS_ENTRY; refusing to replace it (a different key forces an uninstall on the headset)"
        exit 1
    fi

    passphrase="${NEVR_QUEST_KS_IMPORT_PASS:-}"
    [[ -n "$passphrase" ]] || { err "set NEVR_QUEST_KS_IMPORT_PASS to the keystore passphrase"; exit 1; }

    export NEVR_QUEST_KS_PASS="$passphrase"
    if ! keytool -list -keystore "$file" -alias "$ALIAS" -storepass:env NEVR_QUEST_KS_PASS >/dev/null 2>&1; then
        err "cannot read $file with NEVR_QUEST_KS_IMPORT_PASS (wrong passphrase, or no alias $ALIAS)"
        exit 1
    fi
    fp="$(signer_sha256 "$file")" || { err "could not read a SHA-256 fingerprint from $file"; exit 1; }
    [[ -n "$fp" ]] || { err "could not read a SHA-256 fingerprint from $file"; exit 1; }

    b64="$(base64 -w0 "$file")" || { err "cannot read $file"; exit 1; }

    # The passphrase first: a key without its passphrase would be unusable and would block a retry.
    if ! printf '%s\n' "$passphrase" | pass insert -m "$PASS_ENTRY" >/dev/null; then
        err "pass could not store the passphrase at $PASS_ENTRY"
        exit 1
    fi
    if ! printf '%s\n' "$b64" | pass insert -m "$KS_ENTRY" >/dev/null; then
        err "pass could not store the keystore at $KS_ENTRY"
        exit 1
    fi
    printf 'imported: signer sha256 %s\n' "$fp"
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
        import)
            shift
            cmd_import "$@"
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
