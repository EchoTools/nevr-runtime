#!/bin/bash
# verify-server.sh — instrumented server run for VERIFICATION work.
#
# `launch-server.sh` is the operator entry point and is not to be modified
# (AGENTS.md). This script exists so that verification work that needs a
# different flag set, or needs the log as a file rather than a terminal
# stream, does not touch it. Owner-authorised 2026-07-28 ("create a new script
# that you will use").
#
# It deploys the same way launch-server.sh does, then runs echovr.exe with a
# named flag set, captures EVERYTHING to files, waits out the ~15-20s splash
# delay (CLAUDE.md §Startup Timing: judge nothing before 45s), sends CTRL+C,
# and prints a summary read back FROM THE FILES.
#
#   ./verify-server.sh <run-name> [flag-set] [run-seconds]
#
# flag-set:
#   default    -server -headless -no-console   (byte-identical to launch-server.sh)
#   noflag     -server -no-console             (drops the -headless token)
#   upnp       default + -upnp                (flag path to g_upnpEnabled, not config)
#   notelem    default + -no-telemetry         (telemetry explicitly disabled)
#   timestep   default + -fixedtimestep -timestep 120  (the time-step options a server strips, #515)
#
# It deploys into the game install of the main checkout (NEVR_GAME_ROOT overrides), takes the same
# one-run-at-a-time lock as launch-client.sh, and puts every file it deployed back on exit, including
# when it is interrupted.
#
# Never pipe this script's log through grep while it runs — grep block-buffers
# and the run looks hung. Read the files after it exits.
set -uo pipefail

RUN_NAME="${1:?usage: verify-server.sh <run-name> [default|noflag|upnp|notelem|timestep] [seconds]}"
FLAGSET="${2:-default}"
RUN_SECONDS="${3:-100}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO" || exit 1
# shellcheck source=tools/lib/game_install.sh
source tools/lib/game_install.sh
GAME_ROOT=$(resolve_game_root) || exit $?
GAME_DIR="$GAME_ROOT/echovr/bin/win10"
[ -d "$GAME_DIR" ] || { echo "ERROR: no game install at $GAME_DIR (set NEVR_GAME_ROOT to the checkout that has echovr/)" >&2; exit 2; }
SCRATCH_ROOT="${NEVR_RUN_SCRATCH_ROOT:-$NEVR_WORK_ROOT}"
OUT="$SCRATCH_ROOT/server-runs/${RUN_NAME}"

case "$FLAGSET" in
  default) ARGS=(-server -headless -no-console) ;;
  noflag)  ARGS=(-server -no-console) ;;
  # N122/R4: exercises the FLAG path to g_upnpEnabled (boot.cpp) rather than the
  # config-key path (config.cpp). Both set the same global, but only one of them
  # is covered by a run that relies on config.yaml's network.upnp key (N133 moved
  # it out of config.json; issue #21 made config.json optional) — and a flag that silently
  # stopped working would look identical to a config that silently kept working.
  upnp)    ARGS=(-server -headless -no-console -upnp) ;;
  # R5: telemetry is gated on telemetry_uri in config, not on a flag. This flagset
  # exists so the disabled path is exercised deliberately rather than by omission.
  notelem) ARGS=(-server -headless -no-console -no-telemetry) ;;
  # #515: the time-step options GameMainWrapperHook removes from a server's command line before the game
  # parses it. The run's log shows the removal line, the tick-rate line and the clock read-back.
  timestep) ARGS=(-server -headless -no-console -fixedtimestep -timestep 120) ;;
  *) echo "unknown flag-set: $FLAGSET (use default, noflag, upnp, notelem or timestep)" >&2; exit 2 ;;
esac

MIN_SECONDS="${NEVR_VERIFY_MIN_SECONDS:-45}"
POLL_SECONDS="${NEVR_VERIFY_POLL_SECONDS:-5}"
if [ "$RUN_SECONDS" -lt "$MIN_SECONDS" ]; then
  echo "run-seconds must be >= $MIN_SECONDS (CLAUDE.md §Startup Timing: the splash delay alone is ~15-20s)" >&2
  exit 2
fi

# The lock comes before this run's output files are touched: a run that is refused must not empty
# the log of the run that holds the lock.
acquire_game_run_lock
work_dir "$OUT" "Server runs of verify-server.sh: one directory per run name, with the run's log, context and summary." \
  "when the PR that cites a run merges, unless the PR keeps it as evidence."
LOG="$OUT/server.log"
: > "$LOG"

{
  echo "run_name=$RUN_NAME"
  echo "flag_set=$FLAGSET"
  echo "args=${ARGS[*]}"
  echo "run_seconds=$RUN_SECONDS"
  echo "DISPLAY=${DISPLAY:-<unset>}"
  echo "head=$(git -C "$REPO" rev-parse --short HEAD)"
  echo "dirty=$(git -C "$REPO" status --porcelain | wc -l)"
} > "$OUT/context.txt"

# Every file this run deploys: an existing one is saved first and put back on exit, a new one is
# deleted on exit, so the game directory ends as it started even when the run is interrupted.
# The backup lives at one fixed place (the lock allows one run at a time), so a run that was
# SIGKILLed and never restored is found and restored by the next run instead of being mistaken for
# the original.
BACKUP="$SCRATCH_ROOT/verify-server-deploy-backup"
ADDED="$BACKUP/added.txt"
# Puts back every saved file and deletes every added one. Returns 1 if any restore failed; the
# backup is kept in that case.
restore_deployed() {
  local f dst bad=0
  [ -d "$BACKUP" ] || return 0
  if [ -d "$BACKUP/files" ]; then
    while IFS= read -r f; do
      dst="$GAME_DIR/${f#"$BACKUP/files/"}"
      if ! { cp -p "$f" "$dst" && cmp -s "$f" "$dst"; }; then
        echo "ERROR: restoring $dst failed; the original is $f" | tee -a "${LOG:-/dev/null}" >&2
        bad=1
      fi
    done < <(find "$BACKUP/files" -type f)
  fi
  if [ -f "$ADDED" ]; then
    while IFS= read -r dst; do [ -n "$dst" ] && rm -f "$dst"; done < "$ADDED"
  fi
  [ "$bad" -eq 0 ] && rm -rf "$BACKUP"
  return "$bad"
}
if [ -d "$BACKUP" ]; then
  echo "=== restoring files an earlier verify-server.sh run left deployed ===" | tee -a "$LOG"
  restore_deployed || { echo "ERROR: could not restore the earlier run's files; fix $BACKUP by hand" >&2; exit 3; }
fi
work_dir "$BACKUP" "The files verify-server.sh saved from the game install before it deployed over them." \
  "by the script itself when it restores them at the end of the run."
: > "$ADDED"
deploy() {
  local src=$1 dst=$2 rel
  rel="${dst#"$GAME_DIR"/}"
  if [ -e "$dst" ]; then
    mkdir -p "$BACKUP/files/$(dirname "$rel")"
    cp -p "$dst" "$BACKUP/files/$rel" || { echo "ERROR: cannot save $dst before overwriting it" >&2; exit 3; }
  else
    echo "$dst" >> "$ADDED"
  fi
  mkdir -p "$(dirname "$dst")"
  cp -v "$src" "$dst" >> "$LOG" 2>&1 || { echo "ERROR: cannot deploy $src to $dst" >&2; exit 3; }
}
WINE_PID=""
WINE_RUNNING=0
restore() {
  trap - EXIT
  # Only while the game is still ours to stop: after `wait` has reaped it the PID may be reused.
  if [ "$WINE_RUNNING" -eq 1 ]; then kill "$WINE_PID" 2>/dev/null; fi
  if restore_deployed; then
    echo "=== deployed files restored and verified ===" | tee -a "$LOG"
  else
    exit 1
  fi
}
# INT/TERM become a normal exit so the EXIT trap (restore) always runs.
trap 'exit 143' INT TERM
trap restore EXIT

echo "=== Deploying from build/mingw-release/bin/ ===" | tee -a "$LOG"
# The runtime first: the host loads nevr-runtime.dll from the nevr root of the prefix's Windows user (ADR 0012,
# src/core/nevr_root.h), never from the game directory, and stops the game without it. The path is spelled from
# GAME_DIR so the save and restore above reach it like any other deployed file.
NEVR_ROOT_REL="../../.wineprefix/drive_c/users/$(id -un)/AppData/Local/nevr/echovr"
deploy build/mingw-release/bin/nevr-runtime.dll "$GAME_DIR/$NEVR_ROOT_REL/nevr-runtime.dll"
deploy build/mingw-release/bin/BugSplat64.dll "$GAME_DIR/BugSplat64.dll"
if [ -d build/mingw-release/bin/plugins ]; then
  while IFS= read -r f; do deploy "$f" "$GAME_DIR/plugins/${f#build/mingw-release/bin/plugins/}"; done \
    < <(find build/mingw-release/bin/plugins -type f)
fi
# A server with no config.yaml takes the embedded defaults (service_config.cpp BuiltinDefaults, #843), so
# it would start a login bridge against the shipped endpoint. When the install has none, deploy the
# offline-boot config the Windows-VM rig uses (tools/winvm/systest.py OFFLINE_CONFIG_YAML): an explicit
# empty socket_uri, which takes no default, and allow_offline_server. The flag sets reach what they test;
# an install's own config.yaml is used as it is. The runtime finds it at exe-dir/../../_local
# (service_config.cpp), and the restore trap removes it afterwards.
if [ ! -e "$GAME_ROOT/echovr/_local/config.yaml" ]; then
  printf 'services:\n  socket_uri: ""\n  allow_offline_server: true\n' > "$OUT/offline-config.yaml"
  deploy "$OUT/offline-config.yaml" "$GAME_DIR/../../_local/config.yaml"
fi

export WINEPREFIX="$GAME_ROOT/echovr/.wineprefix"

# Window census is by PID attribution, not a raw count: the desktop's window set
# churns independently (steamwebhelper and friends), so a bare count is confounded.
count_game_windows() {
  local pids="$1" n=0 w wpid
  command -v xdotool >/dev/null 2>&1 || { echo "-1"; return; }
  for w in $(xdotool search --onlyvisible "" 2>/dev/null); do
    wpid=$(xdotool getwindowpid "$w" 2>/dev/null) || continue
    case " $pids " in *" $wpid "*) n=$((n+1)) ;; esac
  done
  echo "$n"
}

find "$GAME_ROOT/echovr/" -iname '*.dmp' -newermt '-1 minute' 2>/dev/null | wc -l > "$OUT/dumps_before.txt"

echo "=== Starting echovr.exe ${ARGS[*]} ===" | tee -a "$LOG"
( cd "$GAME_DIR" && exec wine ./echovr.exe "${ARGS[@]}" ) >> "$LOG" 2>&1 9>&- &
WINE_PID=$!
WINE_RUNNING=1

# Poll for the window census across the run rather than sampling once at the end:
# a window that opens and closes before teardown would otherwise go unseen.
MAX_WINDOWS=0
elapsed=0
started_at=$SECONDS
while [ "$elapsed" -lt "$RUN_SECONDS" ]; do
  # One sample interval, ended at once if the game exits first. The sleep runs in the background and
  # the shell waits on it, so an INT/TERM trap runs now and not after the interval (and `wait -n` on
  # the game's PID leaves its exit status for the `wait` below).
  sleep "$POLL_SECONDS" 9>&- &
  sampler=$!
  wait -n "$WINE_PID" "$sampler"
  kill "$sampler" 2>/dev/null
  wait "$sampler" 2>/dev/null
  kill -0 "$WINE_PID" 2>/dev/null || { echo "process exited early at t=$((SECONDS-started_at))s" >> "$OUT/context.txt"; break; }
  elapsed=$((elapsed+POLL_SECONDS))
  pids=$(pgrep -u "$(id -u)" -f "$ECHOVR_CMDLINE" | tr '\n' ' ')
  n=$(count_game_windows "$pids")
  [ "$n" -gt "$MAX_WINDOWS" ] && MAX_WINDOWS="$n"
  echo "t=${elapsed}s game_pids=[$pids] game_windows=$n" >> "$OUT/window-census.txt"
done
echo "max_game_windows=$MAX_WINDOWS" >> "$OUT/context.txt"

echo "=== CTRL+C (SIGINT) ===" | tee -a "$LOG"
for p in $(pgrep -u "$(id -u)" -f "$ECHOVR_CMDLINE"); do kill -INT "$p" 2>/dev/null; done
wait "$WINE_PID"
RC=$?
WINE_RUNNING=0
echo "$RC" > "$OUT/exit_code.txt"

find "$GAME_ROOT/echovr/" -iname '*.dmp' -newermt '-1 minute' 2>/dev/null | wc -l > "$OUT/dumps_after.txt"
ls -t "$GAME_DIR/logs/"*.jsonl 2>/dev/null | head -1 > "$OUT/jsonl_path.txt"

# Summary is read back FROM THE FILES — never from a live pipe.
{
  echo "--- $RUN_NAME (${ARGS[*]}) ---"
  echo "exit_code           = $(cat "$OUT/exit_code.txt")"
  echo "DISPLAY             = ${DISPLAY:-<unset>}"
  echo "max_game_windows    = $MAX_WINDOWS   (-1 = xdotool unavailable)"
  echo "crash_dumps_before  = $(cat "$OUT/dumps_before.txt")"
  echo "crash_dumps_after   = $(cat "$OUT/dumps_after.txt")"
  echo "LOGIN SUCCESS       = $(grep -c 'LOGIN SUCCESS' "$LOG")"
  echo "NSLOBBY registered  = $(grep -c 'registration successful' "$LOG")"
  echo "bridges listening   = $(grep -c 'Proxy listening on' "$LOG")"
  echo "login injections    = $(grep -c 'login injected' "$LOG")"
  echo "graceful shutdown   = $(grep -c 'Graceful shutdown initiated' "$LOG")"
  # Service-endpoint redirect/resolution lines (N133 config-cutover verification).
  # The bridge port is random per run (N39), so a baseline-vs-post diff must
  # normalise ws://127.0.0.1:<port> before comparing.
  echo "service redirects   = $(grep -c 'Service redirect' "$LOG")"
  echo "http redirects      = $(grep -c 'HTTP(S) connection redirected' "$LOG")"
  echo "engine flags lines  = $(grep -c 'engine flags' "$LOG")"
  grep 'engine flags' "$LOG" || true
  echo "log                 = $LOG"
} | tee "$OUT/summary.txt"
