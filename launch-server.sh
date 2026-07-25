#!/bin/bash
set -euo pipefail

# Deploy the current build (not dist/ — dist/ goes stale; build/ is always fresh).
echo "=== Deploying from build/mingw-release/bin/ ==="
cp -v build/mingw-release/bin/BugSplat64.dll echovr/bin/win10/
cp -rv build/mingw-release/bin/modules/* echovr/bin/win10/modules/
if ls build/mingw-release/bin/plugins/*.dll >/dev/null 2>&1; then
  cp -rv build/mingw-release/bin/plugins/* echovr/bin/win10/plugins/
fi

# Ensure _local/config.json is accessible from the game exe directory.
# LoadEarlyConfig searches up to 2 parent dirs: bin/win10/ -> bin/ -> echovr/
mkdir -p echovr/_local
if [ ! -e echovr/_local/config.json ]; then
  echo "No echovr/_local/config.json — link one (e.g., from /mnt/games/CustomLibrary/echovr-vanilla/_local/)"
  exit 1
fi

# Display precondition — Andrew's rule: "either it runs in a Xephyr, or it runs
# server... NO EXCEPTIONS." Verify the display is LIVE before launching; a dead
# display fails deep inside wine as cryptic 'no driver could be loaded' errors
# that were invisible under WINEDEBUG=-all. Fail LOUD and ACTIONABLE here.
XEPHYR_DISPLAY="${XEPHYR_DISPLAY:-:101}"
export DISPLAY="$XEPHYR_DISPLAY"
unset WAYLAND_DISPLAY

if ! xdpyinfo -display "$XEPHYR_DISPLAY" >/dev/null 2>&1; then
  echo "=== DISPLAY $XEPHYR_DISPLAY is DEAD ===" >&2
  echo "" >&2
  echo "The Xephyr at $XEPHYR_DISPLAY is not running. Start it with:" >&2
  echo "  Xephyr $XEPHYR_DISPLAY -ac -noreset -screen 1280x720 &" >&2
  echo "" >&2
  echo "Or if you want the launcher to start it for you, set XEPHYR_AUTOSTART=1." >&2
  if [[ "${XEPHYR_AUTOSTART:-}" == "1" ]]; then
    echo "=== Starting Xephyr $XEPHYR_DISPLAY ===" >&2
    Xephyr "$XEPHYR_DISPLAY" -ac -noreset -screen 1280x720 &
    sleep 2
    if ! xdpyinfo -display "$XEPHYR_DISPLAY" >/dev/null 2>&1; then
      echo "FATAL: Xephyr failed to start on $XEPHYR_DISPLAY" >&2
      exit 1
    fi
  else
    exit 1
  fi
fi

echo "=== Display $XEPHYR_DISPLAY is LIVE ==="
export WINEDLLOVERRIDES="dxgi=b"
export WINEPREFIX=/home/andrew/src/nevr-runtime/echovr/.wineprefix

# Suppress Wine's noisy fixme/dxgi/vkd3d channels WITHOUT suppressing err:,
# which carries the crash/driver-failure diagnostic output. fixme-all turns
# off everything EXCEPT the err: channel — the pipeline-free property is
# preserved (no pipeline) so echovr stays in the terminal's foreground PGID.
export WINEDEBUG=fixme-all

echo "=== Starting echovr.exe ==="
# set -e kills the script before we can capture the exit code. Temporarily
# disable it so the status echo below is reachable on failure.
set +e
cd echovr/bin/win10 && wine ./echovr.exe -server -noconsole 2>&1
exit_code=$?
set -e
if [[ $exit_code -ne 0 ]]; then
  echo "=== echovr.exe exited with code $exit_code ===" >&2
fi
exit $exit_code
