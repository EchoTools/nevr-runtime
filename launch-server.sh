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

export DISPLAY=:101
unset WAYLAND_DISPLAY
export WINEDLLOVERRIDES="dxgi=b"
export WINEPREFIX=/home/andrew/src/nevr-runtime/echovr/.wineprefix

# -server implies -headless (enforced in boot.cpp). -noconsole blocks the game's
# debug console on Wine. The grep strips Wine fixme/dxgi/vkd3d noise.
echo "=== Starting echovr.exe ==="
cd echovr/bin/win10 && wine ./echovr.exe -server -noconsole 2>&1 | grep -v -e fixme -e dxgi -e vkd3d
