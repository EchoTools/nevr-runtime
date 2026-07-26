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
setsid ./launch-server.sh > "$LOG" 2>&1 &
SPID=$!
PGID=$(ps -o pgid= -p $SPID 2>/dev/null | tr -d ' ')
echo "script pid=$SPID pgid=$PGID"

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

# The discriminating check. A dead process releases its port regardless, so the
# port alone cannot tell graceful from crashed. Our handler's first statement is
# an async-signal-safe write() — if that line is absent, it never ran.
if grep -q 'SIGINT received' "$LOG"; then
  echo "PASS: graceful shutdown path ran"
  sed "s/\x1b\[[0-9;]*m//g" "$LOG" | grep -E 'SIGINT received|Graceful shutdown|listener stopped' | cut -c1-140
  exit 0
fi

echo "FAIL: handler never ran — SIGINT did not reach PosixSignalHandler (N87)"
sed "s/\x1b\[[0-9;]*m//g" "$LOG" | grep -E 'exception name=|>>> EXCEPTION|System Info|exited with code' | cut -c1-150
exit 1
