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
#
# Never pipe this script's log through grep while it runs — grep block-buffers
# and the run looks hung. Read the files after it exits.
set -uo pipefail

RUN_NAME="${1:?usage: verify-server.sh <run-name> [default|noflag] [seconds]}"
FLAGSET="${2:-default}"
RUN_SECONDS="${3:-100}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="/var/tmp/work-nevr-runtime/server-runs/${RUN_NAME}"
mkdir -p "$OUT"
LOG="$OUT/server.log"
: > "$LOG"

case "$FLAGSET" in
  default) ARGS=(-server -headless -noconsole) ;;
  noflag)  ARGS=(-server -noconsole) ;;
  *) echo "unknown flag-set: $FLAGSET (use 'default' or 'noflag')" >&2; exit 2 ;;
esac

if [ "$RUN_SECONDS" -lt 45 ]; then
  echo "run-seconds must be >= 45 (CLAUDE.md §Startup Timing: the splash delay alone is ~15-20s)" >&2
  exit 2
fi

{
  echo "run_name=$RUN_NAME"
  echo "flag_set=$FLAGSET"
  echo "args=${ARGS[*]}"
  echo "run_seconds=$RUN_SECONDS"
  echo "DISPLAY=${DISPLAY:-<unset>}"
  echo "head=$(git -C "$REPO" rev-parse --short HEAD)"
  echo "dirty=$(git -C "$REPO" status --porcelain | wc -l)"
} > "$OUT/context.txt"

cd "$REPO" || exit 1

echo "=== Deploying from build/mingw-release/bin/ ===" | tee -a "$LOG"
cp -v build/mingw-release/bin/BugSplat64.dll echovr/bin/win10/ >> "$LOG" 2>&1
cp -rv build/mingw-release/bin/modules/* echovr/bin/win10/modules/ >> "$LOG" 2>&1
if ls build/mingw-release/bin/plugins/*.dll >/dev/null 2>&1; then
  cp -rv build/mingw-release/bin/plugins/* echovr/bin/win10/plugins/ >> "$LOG" 2>&1
fi

export WINEPREFIX="$REPO/echovr/.wineprefix"

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

find "$REPO/echovr" -iname '*.dmp' -newermt '-1 minute' 2>/dev/null | wc -l > "$OUT/dumps_before.txt"

echo "=== Starting echovr.exe ${ARGS[*]} ===" | tee -a "$LOG"
( cd echovr/bin/win10 && exec wine ./echovr.exe "${ARGS[@]}" ) >> "$LOG" 2>&1 &
WINE_PID=$!

# Poll for the window census across the run rather than sampling once at the end:
# a window that opens and closes before teardown would otherwise go unseen.
MAX_WINDOWS=0
elapsed=0
while [ "$elapsed" -lt "$RUN_SECONDS" ]; do
  sleep 5
  elapsed=$((elapsed+5))
  kill -0 "$WINE_PID" 2>/dev/null || { echo "process exited early at t=${elapsed}s" >> "$OUT/context.txt"; break; }
  pids=$(pgrep -f 'echovr\.exe' | tr '\n' ' ')
  n=$(count_game_windows "$pids")
  [ "$n" -gt "$MAX_WINDOWS" ] && MAX_WINDOWS="$n"
  echo "t=${elapsed}s game_pids=[$pids] game_windows=$n" >> "$OUT/window-census.txt"
done
echo "max_game_windows=$MAX_WINDOWS" >> "$OUT/context.txt"

echo "=== CTRL+C (SIGINT) ===" | tee -a "$LOG"
for p in $(pgrep -f 'echovr\.exe'); do kill -INT "$p" 2>/dev/null; done
wait "$WINE_PID"
RC=$?
echo "$RC" > "$OUT/exit_code.txt"

find "$REPO/echovr" -iname '*.dmp' -newermt '-1 minute' 2>/dev/null | wc -l > "$OUT/dumps_after.txt"
ls -t "$REPO/echovr/bin/win10/logs/"*.jsonl 2>/dev/null | head -1 > "$OUT/jsonl_path.txt"

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
  echo "engine flags lines  = $(grep -c 'engine flags' "$LOG")"
  grep 'engine flags' "$LOG" || true
  echo "log                 = $LOG"
} | tee "$OUT/summary.txt"
