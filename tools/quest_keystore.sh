#!/usr/bin/env bash
# The Quest APK signing key, held for one command at a time. The logic is the Go command tools/cmd/quest-keystore
# (ADR 0044): this script builds it from the current source and runs it, and does nothing else.
#
#   just quest-keystore-fingerprint           print the signer's SHA-256
#   tools/quest_keystore.sh check             exit 1 unless both variables name a keystore that opens
#   tools/quest_keystore.sh with <command...> run a command with NEVR_QUEST_KS, NEVR_QUEST_KS_ALIAS and NEVR_QUEST_KS_PASS set
#
# The keystore is named by NEVR_QUEST_KEYSTORE and its passphrase by NEVR_QUEST_KEYSTORE_PASS (an environment variable,
# never an argument). The Go command's own comment says what each mode does.
#
# The binary is built into build/go-tools/, where `just go-tools` puts it, and run as a program of its own. It is not
# started with `go run`: go run exits 1 when the program exits non-zero, which would replace the command's own status,
# and it is not the process that receives the signals a run must answer.
set -euo pipefail

case "${BASH_SOURCE[0]}" in
    */*) here="${BASH_SOURCE[0]%/*}" ;;
    *) here="." ;;
esac
tools="$(cd "$here" && pwd)"
out_dir="$tools/../build/go-tools"
bin="$out_dir/quest-keystore"

if ! command -v go >/dev/null 2>&1; then
    printf 'ERROR: go is required to build the quest-keystore tool (tools/cmd/quest-keystore); install Go 1.25 or later\n' >&2
    exit 1
fi
mkdir -p "$out_dir"
if ! go -C "$tools" build -trimpath -o "$bin" ./cmd/quest-keystore; then
    printf 'ERROR: tools/cmd/quest-keystore does not build, so the Quest keystore cannot be used\n' >&2
    exit 1
fi
exec "$bin" "$@"
