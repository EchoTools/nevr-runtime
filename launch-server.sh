#!/bin/bash
set -euo pipefail

# Deploy the current build (not dist/ — dist/ goes stale; build/ is always fresh).
echo "=== Deploying from build/mingw-release/bin/ ==="
cp -v build/mingw-release/bin/BugSplat64.dll echovr/bin/win10/
# platform_compat and token_auth are statically linked into BugSplat64.dll (2026-08-02).
if ls build/mingw-release/bin/plugins/*.dll >/dev/null 2>&1; then
  cp -rv build/mingw-release/bin/plugins/* echovr/bin/win10/plugins/
fi

export WINEPREFIX=/home/andrew/src/nevr-runtime/echovr/.wineprefix

echo "=== Starting echovr.exe ==="
# set -e kills the script before we can capture the exit code. Temporarily
# disable it so the status echo below is reachable on failure.
set +e
cd echovr/bin/win10 && wine ./echovr.exe -server -headless -noconsole 2>&1
exit_code=$?
set -e
if [[ $exit_code -ne 0 ]]; then
  echo "=== echovr.exe exited with code $exit_code ===" >&2
fi
exit $exit_code
