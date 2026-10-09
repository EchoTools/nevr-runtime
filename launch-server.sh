#!/bin/bash
set -euo pipefail

# The build comes from this checkout; the game install (echovr/) comes from the main checkout, or
# NEVR_GAME_ROOT, so this works from a worktree. One game run at a time (shared with launch-client.sh
# and verify-server.sh): a second run would overlap this one's deployed DLL.
cd "$(dirname "${BASH_SOURCE[0]}")"
# shellcheck source=tools/lib/game_install.sh
source tools/lib/game_install.sh
GAME_ROOT=$(resolve_game_root) || exit $?
GAME_DIR="$GAME_ROOT/echovr/bin/win10"
[[ -d "$GAME_DIR" ]] || { echo "ERROR: no game install at $GAME_DIR (set NEVR_GAME_ROOT)" >&2; exit 2; }
acquire_game_run_lock

# Deploy the current build (not dist/ — dist/ goes stale; build/ is always fresh).
echo "=== Deploying from build/mingw-release/bin/ to $GAME_DIR ==="
cp -v build/mingw-release/bin/BugSplat64.dll "$GAME_DIR/"
# platform_compat and token_auth are statically linked into BugSplat64.dll (2026-08-02).
if ls build/mingw-release/bin/plugins/*.dll >/dev/null 2>&1; then
  mkdir -p "$GAME_DIR/plugins"
  cp -rv build/mingw-release/bin/plugins/* "$GAME_DIR/plugins/"
fi

export WINEPREFIX="$GAME_ROOT/echovr/.wineprefix"

echo "=== Starting echovr.exe ==="
# set -e kills the script before we can capture the exit code. Temporarily
# disable it so the status echo below is reachable on failure. Fd 9 (the run lock) is closed for the
# game so a leftover wineserver cannot hold the lock after this script ends.
set +e
cd "$GAME_DIR" && wine ./echovr.exe -server -headless -noconsole 2>&1 9>&-
exit_code=$?
set -e
if [[ $exit_code -ne 0 ]]; then
  echo "=== echovr.exe exited with code $exit_code ===" >&2
fi
exit $exit_code
