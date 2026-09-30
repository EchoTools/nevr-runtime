#!/bin/bash
# Client system test on the NESTED display only (AGENTS.md "System test after every
# commit"). Deploys the current build, runs the client on Xephyr :101, then judges the
# run from the game's own JSONL log and restores whatever it overwrote.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

GAME_DIR=echovr/bin/win10
SCRATCH=/var/tmp/work-nevr-runtime/client-run-$(date +%Y%m%dT%H%M%S)
LOGDIR="$HOME/src/nevr-runtime/echovr/.wineprefix/drive_c/users/andrew/AppData/Local/EchoVR/logs"

# Nested display only: never the owner's desktop. Unset every Wayland/session
# variable so nothing can fall back to it (gamescope did, see AGENTS.md).
pgrep -f 'Xephyr :101' >/dev/null || { echo "ERROR: Xephyr :101 is not running" >&2; exit 2; }
unset WAYLAND_DISPLAY HYPRLAND_INSTANCE_SIGNATURE XDG_SESSION_TYPE
export DISPLAY=:101
export WINEPREFIX="$HOME/src/nevr-runtime/echovr/.wineprefix"

mkdir -p "$SCRATCH/backup-plugins"
CONSOLE_LOG="$SCRATCH/console.log"
cp -p "$GAME_DIR/BugSplat64.dll" "$SCRATCH/BugSplat64.dll.orig"

# Every plugin DLL we overwrite is backed up first and restored on exit. Test fixtures
# (test_plugin_*.dll) are never deployed.
PLUGINS=()
for p in build/mingw-release/bin/plugins/nevr_*.dll; do
  [[ -f "$p" ]] && PLUGINS+=("$(basename "$p")")
done
for name in "${PLUGINS[@]}"; do
  if [[ -f "$GAME_DIR/plugins/$name" ]]; then
    cp -p "$GAME_DIR/plugins/$name" "$SCRATCH/backup-plugins/$name"
  fi
done

restore() {
  cp -p "$SCRATCH/BugSplat64.dll.orig" "$GAME_DIR/BugSplat64.dll"
  cmp -s "$SCRATCH/BugSplat64.dll.orig" "$GAME_DIR/BugSplat64.dll" \
    && echo "=== original BugSplat64.dll restored and verified ==="
  for name in "${PLUGINS[@]}"; do
    if [[ -f "$SCRATCH/backup-plugins/$name" ]]; then
      cp -p "$SCRATCH/backup-plugins/$name" "$GAME_DIR/plugins/$name"
    else
      rm -f "$GAME_DIR/plugins/$name"
    fi
  done
  wineserver -k
}
trap restore EXIT

echo "=== Deploying from build/mingw-release/bin/ ==="
cp -v build/mingw-release/bin/BugSplat64.dll "$GAME_DIR/"
cmp -s build/mingw-release/bin/BugSplat64.dll "$GAME_DIR/BugSplat64.dll"
sha256sum "$GAME_DIR/BugSplat64.dll"
for name in "${PLUGINS[@]}"; do
  cp -v "build/mingw-release/bin/plugins/$name" "$GAME_DIR/plugins/"
done

echo "=== Starting echovr.exe -noovr -windowed -mp (DISPLAY=$DISPLAY, WAYLAND_DISPLAY unset) ==="
echo "=== Console log: $CONSOLE_LOG ==="

# Evidence that the game really is on the nested display, read from /proc.
(
  sleep 12
  for pid in $(pgrep -x echovr.exe); do
    echo "=== evidence: pid $pid $(tr '\0' '\n' < "/proc/$pid/environ" | grep -E '^(DISPLAY|WAYLAND_DISPLAY)=' | tr '\n' ' ')==="
  done
) &

start=$(date +%s)
set +e
(cd "$GAME_DIR" && wine ./echovr.exe -noovr -windowed -mp) > "$CONSOLE_LOG" 2>&1
exit_code=$?
set -e
wait

if [[ $exit_code -ne 0 ]]; then
  echo "=== echovr.exe exited with code $exit_code ===" >&2
fi

# Judge from the game's own log for THIS run (newest JSONL written since we started).
run_log=""
for f in "$LOGDIR"/nevr-*.jsonl; do
  [[ -f "$f" && $(stat -c %Y "$f") -ge $start ]] && run_log="$f"
done
if [[ -z "$run_log" ]]; then
  echo "FAIL: no game log written since this run started ($LOGDIR)" >&2
  exit 1
fi
count() { grep -c "$1" "$run_log" || test $? -eq 1; }
echo "=== game log: $run_log ==="
echo "logged_in=$(count 'to logged in') in_game=$(count 'to in game') invalid_header=$(count 'invalid header') service_unavailable=$(count 'Service is unavailable')"
if [[ $(count 'to logged in') -eq 0 ]]; then
  echo "FAIL: client never reached logged in" >&2
  exit 1
fi
echo "PASS: client reached logged in"
