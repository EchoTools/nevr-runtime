#!/bin/bash
set -euo pipefail

# Deploy the current build.
echo "=== Deploying from build/mingw-release/bin/ ==="
cp -v build/mingw-release/bin/BugSplat64.dll echovr/bin/win10/
if ls build/mingw-release/bin/plugins/*.dll >/dev/null 2>&1; then
  cp -rv build/mingw-release/bin/plugins/* echovr/bin/win10/plugins/
fi

export DISPLAY=:101
export WINEPREFIX="$HOME/src/nevr-runtime/echovr/.wineprefix"
LOGFILE=/var/tmp/nevr-client-test.log

echo "=== Starting echovr.exe -mp (DISPLAY=$DISPLAY) ==="
echo "=== Log: $LOGFILE ==="

set +e
cd echovr/bin/win10 && wine ./echovr.exe -mp 2>&1 | tee "$LOGFILE"
exit_code=$?
set -e

if [[ $exit_code -ne 0 ]]; then
  echo "=== echovr.exe exited with code $exit_code ===" >&2
fi
exit $exit_code
