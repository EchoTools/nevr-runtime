#!/bin/bash
# Client system test on the NESTED display only (AGENTS.md "System test for runtime and auth changes").
# Verifies the game directory is pristine, deploys the current BugSplat64.dll (the host) and nevr-runtime.dll (the runtime),
# runs the client on display :101 (headless Xvfb; Xephyr when the owner watches), judges the run from the game's own JSONL log, and
# restores the original DLL and removes the runtime it put there.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# shellcheck source=tools/lib/game_install.sh
source tools/lib/game_install.sh
GAME_ROOT=$(resolve_game_root) || exit $?

# --dll PATH deploys that BugSplat64.dll instead of the plugin-dev build (the scenario runner passes
# the mingw-scenario build; see tools/scenario/run_scenario.py). The default is the dev build, not the release one:
# a release host refuses the unsigned nevr-runtime.dll that build makes (#1230, test_preset_unsigned_runtime.py). The runtime is the nevr-runtime.dll beside it,
# or --runtime-dll PATH: the host loads it from the game's own directory, so a run deploys both. --config PATH starts the game with
# `-config PATH` (the game's JSON file) and, when a config.yaml sits in the same directory, `-config-file` naming
# it (the runtime reads config.yaml only from the file `-config-file` names, else from the nevr root), so a run can
# use its own config without touching the game directory. --require-in-game fails the run unless the
# client also reaches "in game" (multiplayer entry), not just "logged in" -- the scenario runner
# passes it with `--game-arg -mp`. --exit-after-login stops the game itself:
# it polls the run's own log until the client reaches "logged in" (PASS), the service stays
# unavailable or --login-timeout seconds pass (FAIL), kills the game, restores the DLL and returns the
# verdict. Without it the script returns only after the game exits (the scenario runner relies on that).
DLL=build/mingw-plugin-dev/bin/BugSplat64.dll
RUNTIME_DLL=""
CONFIG=""
TRACE_EXPORTS=""
EXTRA_GAME_ARGS=()
PLUGINS=()
REQUIRE_IN_GAME=0
EXIT_AFTER_LOGIN=0
LOGIN_TIMEOUT=120
LOGIN_TIMEOUT_SET=0
# The sign-in a run uses (AGENTS.md "Client login test mechanics"): --creds ACCOUNT links the account's stored sign-in
# (tools/lib/game_install.sh creds_store_file) into the nevr root for the run; --device-code starts with no sign-in and
# runs the device-code flow; --save-creds ACCOUNT keeps the sign-in a successful --device-code login makes.
CREDS_GIVEN=0
CREDS_ACCOUNT=""
SAVE_GIVEN=0
SAVE_ACCOUNT=""
DEVICE_CODE=0
# --timeout bounds the whole client run (timeout -s INT -k 15): a client that never logs in is ended and restored.
RUN_TIMEOUT=1800
# The sign-in file this run put in the nevr root: the link to a stored sign-in, or a sign-in a device-code login wrote.
# The restore removes it. Empty until the run makes it, so a restore never removes a file the run did not make.
CLEAN_SIGNIN=""
timed_out=0
# The runtime's own lines that end a device-code flow without a sign-in: a timeout, an expired code, a failure to open or
# show the code, and a flow that cannot start (src/core/device_auth_flow.cpp, src/modules/token-auth/src/token_auth.cpp).
DEVICE_FLOW_FAILED='Device auth timed out after|Device code expired\. Please restart|device authorization stopped because|device code request failed, cannot start|Device authorization operations are unavailable|Cannot run device auth -- not configured'
USAGE="launch-client.sh [--dll PATH] [--runtime-dll PATH] [--config PATH] [--trace-exports LIST] [--game-arg ARG]... [--plugin PATH]... [--require-in-game] [--exit-after-login [--login-timeout SECONDS]] [--timeout SECONDS] (--creds ACCOUNT | --device-code [--save-creds ACCOUNT]) [--print-game-root]"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dll) DLL="${2:?--dll needs a path}"; shift 2 ;;
    --runtime-dll) RUNTIME_DLL="${2:?--runtime-dll needs a path}"; shift 2 ;;
    --config) CONFIG="${2:?--config needs a path}"; shift 2 ;;
    --trace-exports) TRACE_EXPORTS="${2:?--trace-exports needs a list (pnsrad, pnsovr, pnsdemo, all; comma separated)}"; shift 2 ;;
    --game-arg) EXTRA_GAME_ARGS+=("${2:?--game-arg needs one game argument}"); shift 2 ;;
    --plugin) PLUGINS+=("${2:?--plugin needs a path}"); shift 2 ;;
    --require-in-game) REQUIRE_IN_GAME=1; shift ;;
    --exit-after-login) EXIT_AFTER_LOGIN=1; shift ;;
    --login-timeout) LOGIN_TIMEOUT="${2:?--login-timeout needs seconds}"; LOGIN_TIMEOUT_SET=1; shift 2 ;;
    --creds) [[ $# -ge 2 ]] || { echo "ERROR: --creds needs an account name" >&2; exit 2; }; CREDS_GIVEN=1; CREDS_ACCOUNT=$2; shift 2 ;;
    --save-creds) [[ $# -ge 2 ]] || { echo "ERROR: --save-creds needs an account name" >&2; exit 2; }; SAVE_GIVEN=1; SAVE_ACCOUNT=$2; shift 2 ;;
    --device-code) DEVICE_CODE=1; shift ;;
    --timeout) [[ $# -ge 2 ]] || { echo "ERROR: --timeout needs a number of seconds" >&2; exit 2; }; RUN_TIMEOUT=$2; shift 2 ;;
    --print-game-root) echo "$GAME_ROOT"; exit 0 ;;
    -h|--help) echo "usage: $USAGE  (default $DLL)"; exit 0 ;;
    *) echo "unknown argument: $1 (usage: $USAGE)" >&2; exit 2 ;;
  esac
done
# The sign-in choice (AGENTS.md "Client login test mechanics"). A run signs in with a stored account or with the device-code
# flow, not both. --save-creds keeps the sign-in of a --device-code login, so it needs --device-code. Checked before anything
# else, so a refusal here takes no lock and deploys nothing.
if [[ $CREDS_GIVEN -eq 1 && $DEVICE_CODE -eq 1 ]]; then
  echo "ERROR: --creds and --device-code are two sign-ins: pass one (--creds ACCOUNT, or --device-code [--save-creds ACCOUNT])" >&2; exit 2
fi
if [[ $SAVE_GIVEN -eq 1 && $DEVICE_CODE -eq 0 ]]; then
  echo "ERROR: --save-creds keeps the sign-in of a --device-code login: it needs --device-code" >&2; exit 2
fi
if [[ $CREDS_GIVEN -eq 0 && $DEVICE_CODE -eq 0 ]]; then
  echo "ABORT: no sign-in chosen: pass --creds <account> (a stored sign-in) or --device-code (a device-code login, which shows a code to a person; AGENTS.md \"Client login test mechanics\")" >&2
  exit 3
fi
[[ "$RUN_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || { echo "ERROR: --timeout must be a whole number of seconds >= 1 (got $RUN_TIMEOUT)" >&2; exit 2; }
if [[ $DEVICE_CODE -eq 1 ]]; then
  # The device-code flow waits for a person to read and enter the code (5 minutes, src/core/device_auth_flow.cpp), so the
  # run ends at login and the login window covers that code's life plus margin, unless --login-timeout says otherwise.
  EXIT_AFTER_LOGIN=1
  [[ $LOGIN_TIMEOUT_SET -eq 1 ]] || LOGIN_TIMEOUT=420
fi
[[ -f "$DLL" ]] || { echo "ERROR: $DLL does not exist; build it first" >&2; exit 2; }
[[ -n "$RUNTIME_DLL" ]] || RUNTIME_DLL="$(dirname "$DLL")/nevr-runtime.dll"
for plugin in "${PLUGINS[@]}"; do
  [[ -f "$plugin" ]] || { echo "ERROR: --plugin $plugin does not exist" >&2; exit 2; }
done
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

# The stored sign-ins, checked before the lock: a refusal here names the file and takes nothing. STORED is the stored sign-in
# a --creds run links in; SAVE_TO is where a --save-creds run keeps the sign-in it makes (never over an existing one).
STORED=""
if [[ $CREDS_GIVEN -eq 1 ]]; then
  STORED=$(creds_store_file "$CREDS_ACCOUNT") || exit $?
  [[ "$STORED" == /* ]] || { echo "ERROR: NEVR_WORK_ROOT must be an absolute path (the sign-in link needs one): $STORED" >&2; exit 2; }
  check_stored_sign_in "$STORED" "$CREDS_ACCOUNT"
fi
SAVE_TO=""
if [[ $SAVE_GIVEN -eq 1 ]]; then
  SAVE_TO=$(creds_store_file "$SAVE_ACCOUNT") || exit $?
  if [[ -e "$SAVE_TO" || -L "$SAVE_TO" ]]; then
    echo "ABORT: a sign-in is already stored for $SAVE_ACCOUNT ($SAVE_TO); a stored sign-in is not replaced (already stored: choose another account name)" >&2
    exit 3
  fi
fi

acquire_game_run_lock

# Nested display only: never the owner's desktop. Unset every Wayland/session
# variable so nothing can fall back to it (gamescope did, see AGENTS.md).
pgrep -f '([X]vfb|[X]ephyr) :101' >/dev/null || { echo "ERROR: no X server on :101 (start Xvfb :101 -screen 0 1920x1080x24)" >&2; exit 2; }
unset WAYLAND_DISPLAY HYPRLAND_INSTANCE_SIGNATURE XDG_SESSION_TYPE
export DISPLAY=:101
export WINEPREFIX
# A test launch never reaches a host browser (#1447): Wine opens a URL with the xdg-open on PATH, so stubs go first.
block_host_browser

# Pristine state. A run is only meaningful against known files, so any drift aborts
# before anything is deployed. The runtime reads config.yaml (from the nevr root unless -config-file names one) and
# ignores config.json; with no config.yaml in the root and none in _local it must start on its built-in defaults.
drift=0
for f in "$LOCAL_DIR/config.json" "$LOCAL_DIR/config.yaml" "$NEVR_ROOT_DIR/config.yaml"; do
  if [[ -e "$f" || -L "$f" ]]; then echo "DRIFT: $f exists (expected absent)" >&2; drift=1; fi
done
shopt -s nullglob
# Plugins load from the nevr root's plugins\ (ADR 0012). The game folder's plugins\ is not read, but a file in it
# would be moved into the root by the runtime's one-time move, so it counts as drift too.
plugin_files=("$GAME_DIR"/plugins/* "$NEVR_ROOT_DIR"/plugins/*)
shopt -u nullglob
if [[ ${#plugin_files[@]} -ne 0 ]]; then
  echo "DRIFT: a plugins folder is not empty (expected empty: $GAME_DIR/plugins, $NEVR_ROOT_DIR/plugins): ${plugin_files[*]}" >&2; drift=1
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
# A run starts with no sign-in in the game directory. The runtime signs in with the first cache it finds: the nevr root, then
# the exe-relative _local folders (src/core/auth_token.h SelectCredentialCacheLocation). A file there, or a dangling link
# (a sign-in that cannot be read), is refused: the run would sign in as whoever it belongs to, and --creds links its own.
cached=""
for c in "$NEVR_ROOT_DIR" "$GAME_DIR/_local" "$GAME_DIR/../_local" "$LOCAL_DIR"; do
  if [[ -e "$c/.credentials.json" || -L "$c/.credentials.json" ]]; then cached="$c/.credentials.json"; break; fi
done
if [[ -n "$cached" ]]; then
  echo "ABORT: a sign-in is already in the game directory at $cached; a run takes no sign-in of its own (AGENTS.md \"The game install\")" >&2
  exit 3
fi

work_dir "$SCRATCH" "One client run of launch-client.sh: the original BugSplat64.dll it saved and the console log." \
  "when the test that made the run ends; keep the run's log only while a PR cites it."
CONSOLE_LOG="$SCRATCH/console.log"
cp -p "$GAME_DIR/BugSplat64.dll" "$SCRATCH/BugSplat64.dll.orig"

restore() {
  trap - EXIT
  # Every step reports its own failure and the next step still runs (#1469): under set -e a failing command in this trap
  # would end it before the verdict and the Wine server stop. The run exits nonzero when any step failed.
  local failed=0
  if ! cp -p "$SCRATCH/BugSplat64.dll.orig" "$GAME_DIR/BugSplat64.dll"; then
    echo "ERROR: could not restore $GAME_DIR/BugSplat64.dll from $SCRATCH/BugSplat64.dll.orig" >&2; failed=1
  fi
  if ! rm -f -- "$NEVR_ROOT_DIR/nevr-runtime.dll"; then
    echo "ERROR: could not remove $NEVR_ROOT_DIR/nevr-runtime.dll" >&2; failed=1
  fi
  for plugin in "${PLUGINS[@]}"; do
    if ! rm -f -- "$NEVR_ROOT_DIR/plugins/$(basename "$plugin")"; then
      echo "ERROR: could not remove the plugin $NEVR_ROOT_DIR/plugins/$(basename "$plugin")" >&2; failed=1
    fi
  done
  # A --creds run whose runtime refreshed its sign-in has a regular file where the link was: it goes back to the store before
  # the removal below, so the store keeps the refreshed token (#1469). A link, or an identical file, writes nothing.
  if [[ -n "$STORED" ]]; then
    save_back_stored_sign_in "$NEVR_ROOT_DIR/.credentials.json" "$STORED" "$SCRATCH/refreshed-sign-in.credentials.json" || failed=1
  fi
  # The sign-in this run put in the nevr root: the link to a stored sign-in, or a sign-in a device-code login wrote. It goes
  # whatever the run did, so no sign-in is left where the next run would find it (a --save-creds run kept its copy first).
  signin_gone=1
  if [[ -n "$CLEAN_SIGNIN" ]]; then
    if ! rm -f -- "${CLEAN_SIGNIN:?}"; then echo "ERROR: could not remove the run's sign-in at $CLEAN_SIGNIN" >&2; failed=1; fi
    if [[ -e "$CLEAN_SIGNIN" || -L "$CLEAN_SIGNIN" ]]; then signin_gone=0; fi
  fi
  if [[ $failed -eq 0 ]] && cmp -s "$SCRATCH/BugSplat64.dll.orig" "$GAME_DIR/BugSplat64.dll" && [[ ! -e "$NEVR_ROOT_DIR/nevr-runtime.dll" ]] && [[ $signin_gone -eq 1 ]]; then
    echo "=== original BugSplat64.dll restored and verified, nevr-runtime.dll removed from the nevr root ==="
  else
    echo "ERROR: BugSplat64.dll restore failed (or nevr-runtime.dll or the run's sign-in is still in the nevr root, or a step above failed); original is $SCRATCH/BugSplat64.dll.orig" >&2
  fi
  # A cleanup step, not a gate: wineserver exits 1 when no server is left to kill, and as the last command
  # of an EXIT trap under `set -e` that would replace the script's own exit status.
  wineserver -k 9>&- || true
  # The one exit this trap makes: a failed step is nonzero for the whole run, after every step above has run.
  if [[ $failed -ne 0 ]]; then exit 1; fi
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
# --plugin files go into the nevr root's plugins folder, where the runtime loads plugins from (ADR 0012); the restore removes them.
for plugin in "${PLUGINS[@]}"; do
  mkdir -p "$NEVR_ROOT_DIR/plugins"
  cp -v "$plugin" "$NEVR_ROOT_DIR/plugins/"
  cmp -s "$plugin" "$NEVR_ROOT_DIR/plugins/$(basename "$plugin")"
done
sha256sum "$GAME_DIR/BugSplat64.dll" "$NEVR_ROOT_DIR/nevr-runtime.dll"
# The sign-in. The runtime reads its cache from a path it chooses and SelectCredentialCacheLocation takes no path option
# (src/core/auth_token.h), so a --creds run links the stored sign-in into the nevr root: the file stays in the store, and
# the link is removed by the restore on every exit. A device-code run has no sign-in to link; the restore removes the one
# its login writes. The check above found no sign-in in the game directory, so nothing here replaces one.
if [[ -n "$STORED" ]]; then
  ln -s -- "$STORED" "$NEVR_ROOT_DIR/.credentials.json"
  CLEAN_SIGNIN="$NEVR_ROOT_DIR/.credentials.json"
  echo "=== sign-in: the stored sign-in of $CREDS_ACCOUNT is linked into the nevr root for this run (the store keeps the file) ==="
fi
if [[ $DEVICE_CODE -eq 1 ]]; then
  CLEAN_SIGNIN="$NEVR_ROOT_DIR/.credentials.json"
fi

echo "=== Starting echovr.exe -windowed (DISPLAY=$DISPLAY, WAYLAND_DISPLAY unset) ==="
echo "=== Console log: $CONSOLE_LOG ==="
echo "=== host browser blocked: an open the game asks for is recorded in $BROWSER_BLOCK_LOG and nothing opens ==="
if [[ $DEVICE_CODE -eq 1 ]]; then
  echo "=== device code: the login window is $LOGIN_TIMEOUT s. The game shows the code on the nested display (DISPLAY :101), and it expires after 5 minutes. Read it from a screenshot of the game window there; this log masks the code. ==="
fi

# Evidence that the game really is on the nested display, read from /proc.
(
  sleep "${NEVR_EVIDENCE_DELAY:-12}"
  for pid in $(game_pids); do
    echo "=== evidence: pid $pid ppid=$(ps -o ppid= -p "$pid" | tr -d ' ') argv0=$(tr '\0' '\n' < "/proc/$pid/cmdline" | sed -n 1p) $(tr '\0' '\n' < "/proc/$pid/environ" | grep -E '^(DISPLAY|WAYLAND_DISPLAY)=' | tr '\n' ' ')==="
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
  CONFIG_YAML="$(dirname "$CONFIG")/config.yaml"
  if [[ -f "$CONFIG_YAML" ]]; then
    game_args+=(-config-file "Z:${CONFIG_YAML//\//\\}")
    echo "=== game config: $CONFIG, config.yaml: $CONFIG_YAML (-config-file) ==="
  else
    echo "=== game config: $CONFIG (no config.yaml beside it: the run reads the nevr root's, else the built-in defaults) ==="
  fi
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
  # --timeout bounds the client: timeout ends it (INT, then KILL after 15 s) and the verdict says so.
  (cd "$GAME_DIR" && exec timeout -s INT -k 15 "$RUN_TIMEOUT" wine ./echovr.exe "${game_args[@]}") > "$CONSOLE_LOG" 2>&1 9>&- &
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
      # The runtime's device-auth failure (timeout, expired code, no browser to open): the flow will not sign in, so the run ends.
      if grep -qE -- "$DEVICE_FLOW_FAILED" "$cur"; then break; fi
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
  for pid in $(game_pids); do kill "$pid" 2>/dev/null; done
  # timeout's own status: 124 when its timer ended the client, 137 when the client ignored the interrupt and KILL ended it. A
  # bare 137 is not proof of a timeout (a crash also reads 137), so 137 counts only when the run ran for the whole --timeout.
  game_status=0
  wait "$game_pid" 2>/dev/null || game_status=$?
  if [[ $game_status -eq 124 || ( $game_status -eq 137 && $(date +%s) -ge $((start + RUN_TIMEOUT)) ) ]]; then timed_out=1; fi
  timeout 20 wineserver -k 9>&-
  timeout 20 wineserver -w 9>&-
  echo "=== stopped the game after $(( $(date +%s) - start )) s ==="
  exit_code=0
else
  (cd "$GAME_DIR" && timeout -s INT -k 15 "$RUN_TIMEOUT" wine ./echovr.exe "${game_args[@]}") > "$CONSOLE_LOG" 2>&1 9>&-
  exit_code=$?
  if [[ $exit_code -eq 124 || ( $exit_code -eq 137 && $(date +%s) -ge $((start + RUN_TIMEOUT)) ) ]]; then timed_out=1; fi
fi
set -e
wait

if [[ $exit_code -ne 0 ]]; then
  echo "=== echovr.exe exited with code $exit_code ===" >&2
fi
if [[ $timed_out -eq 1 ]]; then
  echo "FAIL: --timeout $RUN_TIMEOUT s elapsed before the client exited (the client was ended and the copy restored)" >&2
  exit 1
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
# The run's log is copied into this run's scratch directory before the verdict (#1297): the test that made the game copy
# deletes it when the run ends, and a log left in the copy goes with it. A copy that does not match the original fails the run.
RUN_LOG_COPY="$SCRATCH/game-log.jsonl"
if ! cp -p -- "$run_log" "$RUN_LOG_COPY" || ! cmp -s -- "$run_log" "$RUN_LOG_COPY"; then
  echo "FAIL: could not copy the game log $run_log to $RUN_LOG_COPY (the log would be lost with the game copy)" >&2
  exit 1
fi
echo "=== game log copied to: $RUN_LOG_COPY ==="
count() { grep -c "$1" "$run_log" || test $? -eq 1; }
echo "=== game log: $run_log ==="
echo "logged_in=$(count 'to logged in') in_game=$(count 'to in game') invalid_header=$(count 'invalid header') service_unavailable=$(count 'Service is unavailable')"
if grep -q 'built-in defaults embedded in this build: (none)' "$run_log"; then
  echo "FAIL: this DLL embeds no service endpoints (its boot log says built-in defaults: (none)); the run cannot be judged" >&2
  exit 1
fi
if [[ $(count 'to logged in') -eq 0 ]]; then
  if grep -qE -- "$DEVICE_FLOW_FAILED" "$run_log"; then
    # The runtime's own device-auth failure line (a timeout, an expired code, no browser): not a service that is down.
    if [[ $DEVICE_CODE -eq 1 ]]; then
      echo "FAIL: the device-code flow ended without a sign-in (the runtime logged a device-auth failure; game log: $run_log)" >&2
    else
      echo "FAIL: the stored sign-in did not log in (the runtime logged a device-auth failure, so it started the device-code flow and that failed; game log: $run_log)" >&2
    fi
    exit 1
  fi
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
# A passing --save-creds run keeps the sign-in its login wrote, at 0600 and never over an existing one. The restore removes
# the copy in the nevr root after this.
if [[ -n "$SAVE_TO" ]]; then
  save_stored_sign_in "$NEVR_ROOT_DIR/.credentials.json" "$SAVE_TO"
fi
