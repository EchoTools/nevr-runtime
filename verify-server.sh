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
#   default    -server -headless -noconsole   (byte-identical to launch-server.sh)
#   noflag     -server -noconsole             (drops the -headless token)
#   upnp       default + -upnp                (flag path to g_upnpEnabled, not config)
#   notelem    default + -notelemetry         (telemetry explicitly disabled)
#
# It deploys into the game install of the main checkout (NEVR_GAME_ROOT overrides), takes the same
# one-run-at-a-time lock as launch-client.sh, and puts every file it deployed back on exit, including
# when it is interrupted.
#
# Never pipe this script's log through grep while it runs — grep block-buffers
# and the run looks hung. Read the files after it exits.
set -uo pipefail

RUN_NAME="${1:?usage: verify-server.sh <run-name> [default|noflag|upnp|notelem] [seconds]}"
FLAGSET="${2:-default}"
RUN_SECONDS="${3:-100}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO" || exit 1
# shellcheck source=tools/lib/game_install.sh
source tools/lib/game_install.sh
GAME_ROOT=$(resolve_game_root) || exit $?
GAME_DIR="$GAME_ROOT/echovr/bin/win10"
[ -d "$GAME_DIR" ] || { echo "ERROR: no game install at $GAME_DIR (set NEVR_GAME_ROOT to the checkout that has echovr/)" >&2; exit 2; }
SCRATCH_ROOT="${NEVR_RUN_SCRATCH_ROOT:-/var/tmp/work-nevr-runtime}"
OUT="$SCRATCH_ROOT/server-runs/${RUN_NAME}"

case "$FLAGSET" in
  default) ARGS=(-server -headless -noconsole) ;;
  noflag)  ARGS=(-server -noconsole) ;;
  # N122/R4: exercises the FLAG path to g_upnpEnabled (boot.cpp) rather than the
  # config-key path (config.cpp). Both set the same global, but only one of them
  # is covered by a run that relies on config.yaml's network.upnp key (N133 moved
  # it out of config.json; issue #21 made config.json optional) — and a flag that silently
  # stopped working would look identical to a config that silently kept working.
  upnp)    ARGS=(-server -headless -noconsole -upnp) ;;
  # R5: telemetry is gated on telemetry_uri in config, not on a flag. This flagset
  # exists so the disabled path is exercised deliberately rather than by omission.
  notelem) ARGS=(-server -headless -noconsole -notelemetry) ;;
  *) echo "unknown flag-set: $FLAGSET (use default, noflag, upnp or notelem)" >&2; exit 2 ;;
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
mkdir -p "$OUT"
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
mkdir -p "$BACKUP"
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
deploy build/mingw-release/bin/BugSplat64.dll "$GAME_DIR/BugSplat64.dll"
if [ -d build/mingw-release/bin/modules ]; then
  while IFS= read -r f; do deploy "$f" "$GAME_DIR/modules/${f#build/mingw-release/bin/modules/}"; done \
    < <(find build/mingw-release/bin/modules -type f)
fi
if [ -d build/mingw-release/bin/plugins ]; then
  while IFS= read -r f; do deploy "$f" "$GAME_DIR/plugins/${f#build/mingw-release/bin/plugins/}"; done \
    < <(find build/mingw-release/bin/plugins -type f)
fi
# A server with no config.yaml gets no embedded defaults (service_config.cpp BuiltinDefaults is
# client-only) and refuses to boot without a login bridge (#16). When the install has none, deploy
# the offline-boot config the Windows-VM rig uses (tools/winvm/systest.py OFFLINE_CONFIG_YAML) so the
# flag sets reach what they test; an install's own config.yaml is used as it is. The runtime finds
# it at exe-dir/../../_local (service_config.cpp), and the restore trap removes it afterwards.
if [ ! -e "$GAME_ROOT/echovr/_local/config.yaml" ]; then
  printf 'services:\n  allow_offline_server: true\n' > "$OUT/offline-config.yaml"
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
while [ "$elapsed" -lt "$RUN_SECONDS" ]; do
  sleep "$POLL_SECONDS" 9>&-
  elapsed=$((elapsed+POLL_SECONDS))
  kill -0 "$WINE_PID" 2>/dev/null || { echo "process exited early at t=${elapsed}s" >> "$OUT/context.txt"; break; }
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
