#!/bin/bash
set -euo pipefail

# Deploy the current build.
echo "=== Deploying from build/mingw-release/bin/ ==="
cp -v build/mingw-release/bin/BugSplat64.dll echovr/bin/win10/
if ls build/mingw-release/bin/plugins/*.dll >/dev/null 2>&1; then
  cp -rv build/mingw-release/bin/plugins/* echovr/bin/win10/plugins/
fi

# Nested display only (AGENTS.md "System test after every commit"): never the
# owner's desktop. Unset WAYLAND_DISPLAY so nothing can fall back to it.
pgrep -f 'Xephyr :101' >/dev/null || { echo "ERROR: Xephyr :101 is not running" >&2; exit 2; }
unset WAYLAND_DISPLAY
export DISPLAY=:101
export WINEPREFIX="$HOME/src/nevr-runtime/echovr/.wineprefix"
LOGFILE=/var/tmp/nevr-client-test.log

echo "=== Starting echovr.exe -noovr -windowed -mp (DISPLAY=$DISPLAY) ==="
echo "=== Log: $LOGFILE ==="

set +e
cd echovr/bin/win10 && wine ./echovr.exe -noovr -windowed -mp 2>&1 | tee "$LOGFILE"
exit_code=$?
set -e

if [[ $exit_code -ne 0 ]]; then
  echo "=== echovr.exe exited with code $exit_code ===" >&2
fi
exit $exit_code
