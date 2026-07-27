#!/bin/bash
# N13/N87 — automated CTRL+C seal.
#
# Reproduces what a terminal CTRL+C actually does: SIGINT to the FOREGROUND
# PROCESS GROUP. `setsid` gives launch-server.sh its own group so
# `kill -INT -<PGID>` mirrors the tty path exactly. launch-server.sh is never
# modified and never bypassed — this signals it from outside.
#
# Why this exists: N13 was sealed on a STRUCTURAL proof (echovr PGID ==
# foreground PGID) plus a port check. Both pass even when shutdown is not
# graceful, because a dead process releases its socket anyway. This harness
# checks the thing that actually distinguishes them — whether our handler ran.
#
# Exit 0 = graceful shutdown observed. 1 = process exited but NOT gracefully.
# 2 = never registered.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH=/var/tmp/work-nevr-runtime
mkdir -p "$SCRATCH"
LOG="$SCRATCH/n13-run.log"
: > "$LOG"

cd "$REPO"
# Never let the SIGINT we are about to send take this harness down with it.
trap '' INT

# `ps -o pgid= -p $!` is NOT the group to signal. In a shell without job control
# (any non-interactive shell — CI, an agent harness, `bash script.sh`) background
# jobs stay in the SHELL's process group, so $! reports the harness's own group
# and `kill -INT -$PGID` signals the harness instead of the server: the harness
# dies, the server never gets the signal, and the run looks like a hang. Have the
# setsid'd child report its own pid — it is the session/group leader by
# construction — and refuse outright to signal our own group.
PIDFILE="$SCRATCH/n13-leader.pid"
rm -f "$PIDFILE"
setsid bash -c 'echo $$ > "$1"; exec ./launch-server.sh' _ "$PIDFILE" > "$LOG" 2>&1 &
for _ in $(seq 1 50); do [[ -s "$PIDFILE" ]] && break; sleep 0.1; done
SPID=$(cat "$PIDFILE" 2>/dev/null)
PGID=$(ps -o pgid= -p "$SPID" 2>/dev/null | tr -d ' ')
MYPGID=$(ps -o pgid= -p $$ | tr -d ' ')
echo "script pid=$SPID pgid=$PGID (harness pgid=$MYPGID)"
if [[ -z "$SPID" || -z "$PGID" || "$PGID" == "$MYPGID" ]]; then
  echo "FAIL: could not isolate the server's process group (pgid='$PGID' harness='$MYPGID')."
  echo "      Refusing to signal — that would kill this harness, not the server."
  [[ -n "${SPID:-}" ]] && kill -9 "$SPID" 2>/dev/null
  exit 2
fi

# CLAUDE.md: >=45s patience from process start before judging liveness.
for i in $(seq 1 120); do
  grep -q 'registration successful' "$LOG" 2>/dev/null && { echo "registered at t=${i}s"; break; }
  sleep 1
done
if ! grep -q 'registration successful' "$LOG"; then
  echo "FAIL: never registered"; kill -9 -"$PGID" 2>/dev/null; exit 2
fi

PORT=$(grep -oE 'ws://127.0.0.1:[0-9]+' "$LOG" | head -1 | grep -oE '[0-9]+$')
echo "bridge port=$PORT  bound_before=$(ss -tlnp 2>/dev/null | grep -c ":$PORT ")"

echo "--- SIGINT -> process group $PGID (what CTRL+C does) ---"
kill -INT -"$PGID"

for i in $(seq 1 30); do
  kill -0 "$SPID" 2>/dev/null || { echo "exited at t=${i}s"; break; }
  sleep 1
done
if kill -0 "$SPID" 2>/dev/null; then
  echo "FAIL: still running 30s after SIGINT"; kill -9 -"$PGID" 2>/dev/null; exit 1
fi
sleep 2
echo "bound_after=$(ss -tlnp 2>/dev/null | grep -c ":$PORT ")"

# The discriminating checks. A dead process releases its port regardless, so the
# port alone cannot tell graceful from crashed. Three independent bits:
#
#  1. handler_ran   — our console ctrl handler emitted its marker. Nothing else
#                     emits it. ('SIGINT received' is the older POSIX-handler
#                     marker, kept because that path is still the native-Windows
#                     one; 'shutdown signal received' is the console-ctrl marker,
#                     which is the only one that fires under Wine — N87 measured
#                     that Wine delivers a tty SIGINT to console ctrl handlers
#                     and never to the CRT signal table.)
#  2. lobby_unreg   — the ServerDB registration was actually torn down. This is
#                     the owner-visible definition of graceful; without it the
#                     lobby lingers server-side.
#  3. clean_exit    — launch-server.sh only prints "exited with code N" for a
#                     NONZERO exit. Its presence means we left through the game's
#                     crash-dump path (code 5), which is not a shutdown.
CLEAN=$(sed "s/\x1b\[[0-9;]*m//g" "$LOG")
handler_ran=0;  grep -qE 'SIGINT received|shutdown signal received' <<<"$CLEAN" && handler_ran=1
lobby_unreg=0;  grep -q  'Unregistered game server'                 <<<"$CLEAN" && lobby_unreg=1
clean_exit=1;   grep -q  'exited with code'                         <<<"$CLEAN" && clean_exit=0
crash_dumps=$(grep -c '=== CRASH DUMP ===' <<<"$CLEAN")
echo "handler_ran=$handler_ran lobby_unregistered=$lobby_unreg clean_exit=$clean_exit crash_dumps=$crash_dumps"

if [[ $handler_ran -eq 1 && $lobby_unreg -eq 1 && $clean_exit -eq 1 ]]; then
  echo "PASS: graceful shutdown path ran"
  grep -E 'shutdown signal received|SIGINT received|deferring to the game|Unregistered game server|Disconnected from ServerDB|teardown complete|listener stopped|Graceful shutdown' <<<"$CLEAN" | cut -c1-160
  exit 0
fi

echo "FAIL: shutdown was not graceful (N87)"
grep -E 'shutdown signal received|Console close signal|exception name=|>>> EXCEPTION|exited with code' <<<"$CLEAN" | cut -c1-160
exit 1
