#!/usr/bin/env bash
# Quest install: a front end over `nevr-install quest install` (ADR 0023, installer/). The core checks the signed
# release manifest, downloads the APK and the game data it names, checks both, plans from what the headset holds,
# installs, and writes the [NEVR.INSTALL] log. This script holds no hash, URL or version of its own.
# usage: tools/quest-install.sh [APK_DIR] [--yes-uninstall] [--version V] [--serial S] [core options]
# The core is $NEVR_INSTALL, else `nevr-install` on PATH. Needs adb, aapt2 and apksigner on PATH (the core checks the
# APK against the manifest before it installs). Exit code is the core's: 0 installed or nothing to do, 1 failed with
# nothing changed, 2 needs the player, 3 restored.
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }

if [ "${1-}" = "yes" ]; then
    die "the 'yes' argument is now --yes-uninstall (it lets the core uninstall a differently signed app and delete its app data)"
fi

core=${NEVR_INSTALL:-}
if [ -z "$core" ]; then
    core=$(command -v nevr-install || true)
fi
[ -n "$core" ] && [ -x "$core" ] || die "nevr-install not found: build it with `just build-installer` and put it on PATH, or set NEVR_INSTALL"

# A directory as the first argument is a local tester APK folder: the core's --artifact-dir (one APK and its sha256
# sidecar; the core logs every step as an unverified dev artifact). This script is a developer tool; the installers a
# player runs never pass it.
if [ -n "${1-}" ] && [ "${1#-}" = "$1" ] && [ -d "$1" ]; then
    artifact_dir=$1
    shift
    set -- --artifact-dir "$artifact_dir" "$@"
fi

exec "$core" quest install "$@"
