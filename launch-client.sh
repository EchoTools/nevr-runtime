#!/bin/bash

# Copy built DLLs into the game directory.
cp -v dist/nevr-runtime*/* echovr/bin/win10

# Ensure _local/config.json is accessible from the game exe directory.
# LoadEarlyConfig searches up to 2 parent dirs: bin/win10/ -> bin/ -> echovr/
mkdir -p echovr/_local
if [ ! -e echovr/_local/config.json ]; then
  echo "No echovr/_local/config.json — link one (e.g., from /mnt/games/CustomLibrary/echovr-vanilla/_local/)"
  exit 1
fi

export DISPLAY=:101
export WINEDLLOVERRIDES="dxgi=b"
export WINEPREFIX=/home/andrew/src/nevr-runtime/.wineprefix

cd echovr/bin/win10 && wine ./echovr.exe -windowed -mp 2>&1 | grep -v -e fixme -e dxgi
