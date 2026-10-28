#!/bin/bash
# Client system test on the NESTED display only (AGENTS.md "System test after every
# commit"). Verifies the game directory is pristine, deploys the current BugSplat64.dll (the host) and nevr-runtime.dll (the runtime),
# runs the client on display :101 (headless Xvfb; Xephyr when the owner watches), judges the run from the game's own JSONL log, and
# restores the original DLL and removes the runtime it put there.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# shellcheck source=tools/lib/game_install.sh
source tools/lib/game_install.sh
GAME_ROOT=$(resolve_game_root) || exit $?

# --dll PATH deploys that BugSplat64.dll instead of the release build (the scenario runner passes
# the mingw-scenario build; see tools/scenario/run_scenario.py). The runtime is the nevr-runtime.dll beside it,
# or --runtime-dll PATH: the host loads it from the game's own directory, so a run deploys both. --config PATH starts the game with
# `-config PATH` (a JSON file; the runtime reads config.yaml from the same directory), so a run can
# use its own config without touching the game directory. --require-in-game fails the run unless the
# client also reaches "in game" (multiplayer entry), not just "logged in" -- the scenario runner
# passes it with `--game-arg -mp`. --exit-after-login stops the game itself:
# it polls the run's own log until the client reaches "logged in" (PASS), the service stays
# unavailable or --login-timeout seconds pass (FAIL), kills the game, restores the DLL and returns the
# verdict. Without it the script returns only after the game exits (the scenario runner relies on that).
DLL=build/mingw-release/bin/BugSplat64.dll
RUNTIME_DLL=""
CONFIG=""
TRACE_EXPORTS=""
EXTRA_GAME_ARGS=()
REQUIRE_IN_GAME=0
EXIT_AFTER_LOGIN=0
LOGIN_TIMEOUT=120
USAGE="launch-client.sh [--dll PATH] [--runtime-dll PATH] [--config PATH] [--trace-exports LIST] [--game-arg ARG]... [--require-in-game] [--exit-after-login [--login-timeout SECONDS]] [--print-game-root]"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dll) DLL="${2:?--dll needs a path}"; shift 2 ;;
    --runtime-dll) RUNTIME_DLL="${2:?--runtime-dll needs a path}"; shift 2 ;;
    --config) CONFIG="${2:?--config needs a path}"; shift 2 ;;
    --trace-exports) TRACE_EXPORTS="${2:?--trace-exports needs a list (pnsrad, pnsovr, pnsdemo, all; comma separated)}"; shift 2 ;;
    --game-arg) EXTRA_GAME_ARGS+=("${2:?--game-arg needs one game argument}"); shift 2 ;;
    --require-in-game) REQUIRE_IN_GAME=1; shift ;;
    --exit-after-login) EXIT_AFTER_LOGIN=1; shift ;;
    --login-timeout) LOGIN_TIMEOUT="${2:?--login-timeout needs seconds}"; shift 2 ;;
    --print-game-root) echo "$GAME_ROOT"; exit 0 ;;
    -h|--help) echo "usage: $USAGE  (default $DLL)"; exit 0 ;;
    *) echo "unknown argument: $1 (usage: $USAGE)" >&2; exit 2 ;;
  esac
done
[[ -f "$DLL" ]] || { echo "ERROR: $DLL does not exist; build it first" >&2; exit 2; }
[[ -n "$RUNTIME_DLL" ]] || RUNTIME_DLL="$(dirname "$DLL")/nevr-runtime.dll"
[[ -f "$RUNTIME_DLL" ]] || { echo "ERROR: $RUNTIME_DLL does not exist; the host loads it from the game directory, build it first" >&2; exit 2; }
# 45 s is the minimum patience from process start (AGENTS.md "Startup Timing"): the splash alone is 15-20 s.
# NEVR_LOGIN_MIN_SECONDS lowers it for the script's own tests only.
LOGIN_MIN_SECONDS="${NEVR_LOGIN_MIN_SECONDS:-45}"
if [[ $EXIT_AFTER_LOGIN -eq 1 ]]; then
  [[ "$LOGIN_TIMEOUT" =~ ^[0-9]+$ && "$LOGIN_TIMEOUT" -ge "$LOGIN_MIN_SECONDS" ]] || {
    echo "ERROR: --login-timeout must be a whole number of seconds >= $LOGIN_MIN_SECONDS (the splash alone takes 15-20 s)" >&2; exit 2; }
fi

GAME_DIR="$GAME_ROOT/echovr/bin/win10"
LOCAL_DIR="$GAME_ROOT/echovr/_local"
[[ -d "$GAME_DIR" ]] || { echo "ERROR: no game install at $GAME_DIR (set NEVR_GAME_ROOT to the checkout that has echovr/)" >&2; exit 2; }
SCRATCH="${NEVR_RUN_SCRATCH_ROOT:-$NEVR_WORK_ROOT}/client-run-$(date +%Y%m%dT%H%M%S)"
WINEPREFIX="$GAME_ROOT/echovr/.wineprefix"
# The nevr root of the prefix's Windows user (src/core/nevr_root.h, ADR 0012): the run log, the caches and the sign-in cache live under it.
NEVR_ROOT_DIR="$WINEPREFIX/drive_c/users/$(id -un)/AppData/Local/nevr/echovr"
LOGDIR="$NEVR_ROOT_DIR/logs"

acquire_game_run_lock

# Nested display only: never the owner's desktop. Unset every Wayland/session
# variable so nothing can fall back to it (gamescope did, see AGENTS.md).
pgrep -f '([X]vfb|[X]ephyr) :101' >/dev/null || { echo "ERROR: no X server on :101 (start Xvfb :101 -screen 0 1920x1080x24)" >&2; exit 2; }
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
# The host loads nevr-runtime.dll from the nevr root, never from the game install (ADR 0012). One in either place
# is a leftover of another run (or a file the host would never load), which the restore step below would delete
# as its own.
for stale in "$GAME_DIR/nevr-runtime.dll" "$NEVR_ROOT_DIR/nevr-runtime.dll"; do
  if [[ -e "$stale" || -L "$stale" ]]; then
    echo "DRIFT: $stale exists (expected absent)" >&2; drift=1
  fi
done
if [[ $drift -ne 0 ]]; then echo "ABORT: game directory is not pristine" >&2; exit 3; fi
# The runtime takes the nevr root's cache, then (until the one-time move has taken it) the first in exe-relative order:
# _local, ../_local, ../../_local (src/core/auth_token.h SelectCredentialCacheLocation); a device-code login with none
# saves to the root.
cached=""
for c in "$NEVR_ROOT_DIR" "$GAME_DIR/_local" "$GAME_DIR/../_local" "$LOCAL_DIR"; do
  if [[ -f "$c/.credentials.json" ]]; then cached="$c/.credentials.json"; break; fi
done
if [[ -n "$cached" ]]; then
  echo "=== note: cached credentials present ($cached); this run uses the cached flow ==="
else
  echo "=== note: no cached credentials; this run exercises the full device-code flow ==="
fi

work_dir "$SCRATCH" "One client run of launch-client.sh: the original BugSplat64.dll it saved and the console log." \
  "when the test that made the run ends; keep the run's log only while a PR cites it."
CONSOLE_LOG="$SCRATCH/console.log"
cp -p "$GAME_DIR/BugSplat64.dll" "$SCRATCH/BugSplat64.dll.orig"

restore() {
  trap - EXIT
  cp -p "$SCRATCH/BugSplat64.dll.orig" "$GAME_DIR/BugSplat64.dll"
  rm -f "$NEVR_ROOT_DIR/nevr-runtime.dll"
  if cmp -s "$SCRATCH/BugSplat64.dll.orig" "$GAME_DIR/BugSplat64.dll" && [[ ! -e "$NEVR_ROOT_DIR/nevr-runtime.dll" ]]; then
    echo "=== original BugSplat64.dll restored and verified, nevr-runtime.dll removed from the nevr root ==="
  else
    echo "ERROR: BugSplat64.dll restore failed (or nevr-runtime.dll is still in the nevr root); original is $SCRATCH/BugSplat64.dll.orig" >&2
  fi
  # A cleanup step, not a gate: wineserver exits 1 when no server is left to kill, and as the last command
  # of an EXIT trap under `set -e` that would replace the script's own exit status.
  wineserver -k 9>&- || true
}
# INT/TERM become a normal exit so the EXIT trap (restore) always runs.
trap 'exit 143' INT TERM
trap restore EXIT

echo "=== Deploying $DLL ==="
# The runtime first, into the nevr root where the host looks for it: until the host is replaced nothing loads it.
mkdir -p "$NEVR_ROOT_DIR"
cp -v "$RUNTIME_DLL" "$NEVR_ROOT_DIR/nevr-runtime.dll"
cmp -s "$RUNTIME_DLL" "$NEVR_ROOT_DIR/nevr-runtime.dll"
cp -v "$DLL" "$GAME_DIR/BugSplat64.dll"
cmp -s "$DLL" "$GAME_DIR/BugSplat64.dll"
sha256sum "$GAME_DIR/BugSplat64.dll" "$NEVR_ROOT_DIR/nevr-runtime.dll"

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
# it is the game's no-headset mode. A client is not meant to run with -noovr (a "VR bypass").
# A player's client reaches multiplayer from the menu's own Play action; a scenario has no hand, so
# it passes the stock flag that begins multiplayer itself: `--game-arg -mp` (#674). The game's syntax
# has both: `-mp\0` sits in echovr.exe's CLI table at file offset 0x16d0d44, beside -capture and
# -dumpstats; `-windowed` is the runtime's own flag (src/runtime/lifecycle/cli.cpp).
game_args=(-windowed)
if [[ ${#EXTRA_GAME_ARGS[@]} -gt 0 ]]; then
  game_args+=("${EXTRA_GAME_ARGS[@]}")
  echo "=== extra game arguments: ${EXTRA_GAME_ARGS[*]} ==="
fi
if [[ -n "$CONFIG" ]]; then
  [[ -f "$CONFIG" ]] || { echo "ERROR: --config $CONFIG does not exist" >&2; exit 2; }
  game_args+=(-config "Z:${CONFIG//\//\\}")
  echo "=== game config: $CONFIG (config.yaml from its directory) ==="
fi
if [[ -n "$TRACE_EXPORTS" ]]; then
  [[ "$TRACE_EXPORTS" =~ ^[A-Za-z,.]+$ ]] || { echo "ERROR: --trace-exports takes a comma separated list of pnsrad, pnsovr, pnsdemo, all" >&2; exit 2; }
  game_args+=(-trace-exports "$TRACE_EXPORTS")
  echo "=== tracing platform DLL exports: $TRACE_EXPORTS (docs/reference/export-tracer.md) ==="
fi
# The newest game log THIS run wrote: a file that did not exist when the run started (names carry a
# millisecond timestamp, so they are unique per run) and was modified since. Judging by mtime alone
# let a previous run's log, last written in the same second the run started, decide the verdict. The host's
# nevr-bootstrap.jsonl lives in the same folder and is new in a root the host has not written to before.
pre_logs=$(for f in "$LOGDIR"/nevr-*.jsonl; do [[ -e "$f" ]] && printf '%s\n' "$f"; done || true)
newest_run_log() {
  local f found=""
  for f in "$LOGDIR"/nevr-*.jsonl; do
    [[ -f "$f" && $(stat -c %Y "$f") -ge $start ]] || continue
    [[ "$f" == */nevr-bootstrap.jsonl ]] && continue  # the host's record, in the same folder: not the game's log
    grep -qxF -- "$f" <<<"$pre_logs" && continue
    found="$f"
  done
  printf '%s' "$found"
}
if [[ $EXIT_AFTER_LOGIN -eq 1 ]]; then
  (cd "$GAME_DIR" && wine ./echovr.exe "${game_args[@]}") > "$CONSOLE_LOG" 2>&1 9>&- &
  game_pid=$!
  # $start is whole seconds (it is compared with file mtimes), so start + timeout can fall up to a
  # second short of the timeout. The deadline is measured in microseconds from the launch instead.
  launched_us=${EPOCHREALTIME/./}
  deadline_us=$((launched_us + LOGIN_TIMEOUT * 1000000))
  poll="${NEVR_LOGIN_POLL_SECONDS:-2}"
  while kill -0 "$game_pid" 2>/dev/null && [[ ${EPOCHREALTIME/./} -lt $deadline_us ]]; do
    cur=$(newest_run_log)
    if [[ -n "$cur" ]]; then
      if grep -q 'to logged in' "$cur"; then break; fi
      # A run that logs in never logs "rad15_live failed" (0 of 459 logged-in runs in the logs this
      # was measured on) and the stuck ones log it again and again (3 or more in 11 of 479, up to
      # 2038), so three of those, or three "Service is unavailable", is a service that is not coming.
      if [[ $(grep -c 'rad15_live failed' "$cur" || true) -ge 3 ]]; then break; fi
      if [[ $(grep -c 'Service is unavailable' "$cur" || true) -ge 3 ]]; then break; fi
      # A DLL that embeds no service endpoints can never log in; do not wait out the deadline.
      if grep -q 'built-in defaults embedded in this build: (none)' "$cur"; then break; fi
    fi
    sleep "$poll" 9>&-
  done
  # Verdict reached (or the game died or the deadline passed): stop the game, and make sure the Wine
  # server is gone too, so nothing still has BugSplat64.dll or nevr-runtime.dll mapped when the restore trap copies the
  # original back over it.
  kill "$game_pid" 2>/dev/null
  for pid in $(pgrep -u "$(id -u)" -f "$ECHOVR_CMDLINE"); do kill "$pid" 2>/dev/null; done
  wait "$game_pid" 2>/dev/null
  timeout 20 wineserver -k 9>&-
  timeout 20 wineserver -w 9>&-
  echo "=== stopped the game after $(( $(date +%s) - start )) s ==="
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
  # The runtime writes the game log, so a host that stopped the game before the runtime ran leaves none; the host's
  # own record says why (#1122).
  if cause=$(host_stop_cause "$LOGDIR" "$start"); then echo "FAIL: $cause" >&2; fi
  exit 1
fi
count() { grep -c "$1" "$run_log" || test $? -eq 1; }
echo "=== game log: $run_log ==="
echo "logged_in=$(count 'to logged in') in_game=$(count 'to in game') invalid_header=$(count 'invalid header') service_unavailable=$(count 'Service is unavailable')"
if grep -q 'built-in defaults embedded in this build: (none)' "$run_log"; then
  echo "FAIL: this DLL embeds no service endpoints (its boot log says built-in defaults: (none)); the run cannot be judged" >&2
  exit 1
fi
if [[ $(count 'to logged in') -eq 0 ]]; then
  echo "FAIL: client never reached logged in" >&2
  exit 1
fi
echo "PASS: client reached logged in"
if [[ $REQUIRE_IN_GAME -eq 1 ]]; then
  if [[ $(count 'to in game') -eq 0 ]]; then
    echo "FAIL: client never reached in game" >&2
    exit 1
  fi
  echo "PASS: client reached in game (multiplayer entry)"
fi
