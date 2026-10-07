#!/bin/bash
# Client system test on the NESTED display only (AGENTS.md "System test after every
# commit"). Verifies the game directory is pristine, deploys the current BugSplat64.dll,
# runs the client on Xephyr :101, judges the run from the game's own JSONL log, and
# restores the original DLL.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# shellcheck source=tools/lib/game_install.sh
source tools/lib/game_install.sh
GAME_ROOT=$(resolve_game_root) || exit $?

# --dll PATH deploys that BugSplat64.dll instead of the release build (the scenario runner passes
# the mingw-scenario build; see tools/scenario/run_scenario.py). --config PATH starts the game with
# `-config PATH` (a JSON file; the runtime reads config.yaml from the same directory), so a run can
# use its own config without touching the game directory. --exit-after-login stops the game itself:
# it polls the run's own log until the client reaches "logged in" (PASS), the service stays
# unavailable or --login-timeout seconds pass (FAIL), kills the game, restores the DLL and returns the
# verdict. Without it the script returns only after the game exits (the scenario runner relies on that).
DLL=build/mingw-release/bin/BugSplat64.dll
CONFIG=""
EXIT_AFTER_LOGIN=0
LOGIN_TIMEOUT=120
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dll) DLL="${2:?--dll needs a path}"; shift 2 ;;
    --config) CONFIG="${2:?--config needs a path}"; shift 2 ;;
    --exit-after-login) EXIT_AFTER_LOGIN=1; shift ;;
    --login-timeout) LOGIN_TIMEOUT="${2:?--login-timeout needs seconds}"; shift 2 ;;
    --print-game-root) echo "$GAME_ROOT"; exit 0 ;;
    -h|--help) echo "usage: launch-client.sh [--dll PATH] [--config PATH] [--exit-after-login [--login-timeout SECONDS]] [--print-game-root]  (default $DLL)"; exit 0 ;;
    *) echo "unknown argument: $1 (usage: launch-client.sh [--dll PATH] [--config PATH] [--exit-after-login [--login-timeout SECONDS]] [--print-game-root])" >&2; exit 2 ;;
  esac
done
[[ -f "$DLL" ]] || { echo "ERROR: $DLL does not exist; build it first" >&2; exit 2; }
# 45 s is the minimum patience from process start (AGENTS.md "Startup Timing"): the splash alone is 15-20 s.
LOGIN_MIN_SECONDS="${NEVR_LOGIN_MIN_SECONDS:-45}"
[[ "$LOGIN_TIMEOUT" =~ ^[0-9]+$ && "$LOGIN_TIMEOUT" -ge "$LOGIN_MIN_SECONDS" ]] || {
  echo "ERROR: --login-timeout must be a whole number of seconds >= $LOGIN_MIN_SECONDS (the splash alone takes 15-20 s)" >&2; exit 2; }

GAME_DIR="$GAME_ROOT/echovr/bin/win10"
LOCAL_DIR="$GAME_ROOT/echovr/_local"
[[ -d "$GAME_DIR" ]] || { echo "ERROR: no game install at $GAME_DIR (set NEVR_GAME_ROOT to the checkout that has echovr/)" >&2; exit 2; }
SCRATCH="${NEVR_RUN_SCRATCH_ROOT:-/var/tmp/work-nevr-runtime}/client-run-$(date +%Y%m%dT%H%M%S)"
WINEPREFIX="$GAME_ROOT/echovr/.wineprefix"
LOGDIR="$WINEPREFIX/drive_c/users/$(id -un)/AppData/Local/EchoVR/logs"

acquire_game_run_lock

# Nested display only: never the owner's desktop. Unset every Wayland/session
# variable so nothing can fall back to it (gamescope did, see AGENTS.md).
pgrep -f 'Xephyr :101' >/dev/null || { echo "ERROR: Xephyr :101 is not running" >&2; exit 2; }
unset WAYLAND_DISPLAY HYPRLAND_INSTANCE_SIGNATURE XDG_SESSION_TYPE
export DISPLAY=:101
export WINEPREFIX

# Pristine state. A run is only meaningful against known files, so any drift aborts
# before anything is deployed. The runtime reads config.yaml and ignores config.json;
# with neither present it must start on its built-in defaults.
drift=0
for f in "$LOCAL_DIR/config.json" "$LOCAL_DIR/config.yaml"; do
  if [[ -e "$f" || -L "$f" ]]; then echo "DRIFT: $f exists (expected absent)" >&2; drift=1; fi
done
shopt -s nullglob
plugin_files=("$GAME_DIR"/plugins/*)
shopt -u nullglob
if [[ ${#plugin_files[@]} -ne 0 ]]; then
  echo "DRIFT: $GAME_DIR/plugins is not empty: ${plugin_files[*]}" >&2; drift=1
fi
if [[ $drift -ne 0 ]]; then echo "ABORT: game directory is not pristine" >&2; exit 3; fi
# The runtime takes the first cache in exe-relative order: _local, ../_local, ../../_local
# (src/core/auth_token.h SelectCredentialCacheLocation); a device-code login with none saves to
# $GAME_DIR/_local.
cached=""
for c in "$GAME_DIR/_local" "$GAME_DIR/../_local" "$LOCAL_DIR"; do
  if [[ -f "$c/.credentials.json" ]]; then cached="$c/.credentials.json"; break; fi
done
if [[ -n "$cached" ]]; then
  echo "=== note: cached credentials present ($cached); this run uses the cached flow ==="
else
  echo "=== note: no cached credentials; this run exercises the full device-code flow ==="
fi

mkdir -p "$SCRATCH"
CONSOLE_LOG="$SCRATCH/console.log"
cp -p "$GAME_DIR/BugSplat64.dll" "$SCRATCH/BugSplat64.dll.orig"

restore() {
  trap - EXIT
  cp -p "$SCRATCH/BugSplat64.dll.orig" "$GAME_DIR/BugSplat64.dll"
  if cmp -s "$SCRATCH/BugSplat64.dll.orig" "$GAME_DIR/BugSplat64.dll"; then
    echo "=== original BugSplat64.dll restored and verified ==="
  else
    echo "ERROR: BugSplat64.dll restore failed; original is $SCRATCH/BugSplat64.dll.orig" >&2
  fi
  wineserver -k 9>&-
}
# INT/TERM become a normal exit so the EXIT trap (restore) always runs.
trap 'exit 143' INT TERM
trap restore EXIT

echo "=== Deploying $DLL ==="
cp -v "$DLL" "$GAME_DIR/BugSplat64.dll"
cmp -s "$DLL" "$GAME_DIR/BugSplat64.dll"
sha256sum "$GAME_DIR/BugSplat64.dll"

echo "=== Starting echovr.exe -windowed (DISPLAY=$DISPLAY, WAYLAND_DISPLAY unset) ==="
echo "=== Console log: $CONSOLE_LOG ==="

# Evidence that the game really is on the nested display, read from /proc.
(
  sleep "${NEVR_EVIDENCE_DELAY:-12}"
  for pid in $(pgrep -u "$(id -u)" -f "$ECHOVR_CMDLINE"); do
    echo "=== evidence: pid $pid $(tr '\0' '\n' < "/proc/$pid/environ" | grep -E '^(DISPLAY|WAYLAND_DISPLAY)=' | tr '\n' ' ')==="
  done
) 9>&- &

start=$(date +%s)
set +e
# -windowed alone (owner, 2026-10-03: "just -windowed"; "-windowed is basically -novr (not -noovr)"):
# it is the game's no-headset mode. A client is not meant to run with -noovr (added in e449958 as a
# "VR bypass"); -mp has no reader in the runtime and no string in echovr.exe
# (issue #45).
game_args=(-windowed)
if [[ -n "$CONFIG" ]]; then
  [[ -f "$CONFIG" ]] || { echo "ERROR: --config $CONFIG does not exist" >&2; exit 2; }
  game_args+=(-config "Z:${CONFIG//\//\\}")
  echo "=== game config: $CONFIG (config.yaml from its directory) ==="
fi
# The newest game log written since this run started.
newest_run_log() {
  local f found=""
  for f in "$LOGDIR"/nevr-*.jsonl; do
    [[ -f "$f" && $(stat -c %Y "$f") -ge $start ]] && found="$f"
  done
  printf '%s' "$found"
}
if [[ $EXIT_AFTER_LOGIN -eq 1 ]]; then
  (cd "$GAME_DIR" && wine ./echovr.exe "${game_args[@]}") > "$CONSOLE_LOG" 2>&1 9>&- &
  game_pid=$!
  deadline=$((start + LOGIN_TIMEOUT))
  poll="${NEVR_LOGIN_POLL_SECONDS:-2}"
  while kill -0 "$game_pid" 2>/dev/null && [[ $(date +%s) -lt $deadline ]]; do
    cur=$(newest_run_log)
    if [[ -n "$cur" ]]; then
      if grep -q 'to logged in' "$cur"; then break; fi
      if [[ $(grep -c 'Service is unavailable' "$cur" || true) -ge 3 ]]; then break; fi
    fi
    sleep "$poll" 9>&-
  done
  # Verdict reached (or the game died or the deadline passed): stop the game. The restore trap runs
  # wineserver -k as well; the explicit kill lets the wait below return.
  kill "$game_pid" 2>/dev/null
  for pid in $(pgrep -u "$(id -u)" -f "$ECHOVR_CMDLINE"); do kill "$pid" 2>/dev/null; done
  wait "$game_pid" 2>/dev/null
  exit_code=0
else
  (cd "$GAME_DIR" && wine ./echovr.exe "${game_args[@]}") > "$CONSOLE_LOG" 2>&1 9>&-
  exit_code=$?
fi
set -e
wait

if [[ $exit_code -ne 0 ]]; then
  echo "=== echovr.exe exited with code $exit_code ===" >&2
fi

# Judge from the game's own log for THIS run (newest JSONL written since we started).
run_log=$(newest_run_log)
if [[ -z "$run_log" ]]; then
  echo "FAIL: no game log written since this run started ($LOGDIR)" >&2
  exit 1
fi
count() { grep -c "$1" "$run_log" || test $? -eq 1; }
echo "=== game log: $run_log ==="
echo "logged_in=$(count 'to logged in') in_game=$(count 'to in game') invalid_header=$(count 'invalid header') service_unavailable=$(count 'Service is unavailable')"
if grep -q 'built-in defaults embedded in this build: (none)' "$run_log"; then
  echo "FAIL: this DLL embeds no service endpoints (built without the production .env); the run cannot be judged" >&2
  exit 1
fi
if [[ $(count 'to logged in') -eq 0 ]]; then
  echo "FAIL: client never reached logged in" >&2
  exit 1
fi
echo "PASS: client reached logged in"
