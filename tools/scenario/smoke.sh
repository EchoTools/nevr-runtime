#!/usr/bin/env bash
# The scheduled NEVR scenario smoke: one client boot that proves the three things every social
# scenario assumes, in order, and fails LOUD when any is missing. This is the check that would have
# caught the 2026-10-03 regression where launch-client.sh dropped -mp and the client never entered
# multiplayer, while the scenario suite sat outside CI.
#
#   1. the client reaches "in game" (multiplayer entry; launch-client.sh --require-in-game)
#   2. the mingw-scenario control endpoint logs its port (the fire/inject path is live)
#   3. one fire -> expect_log slot=NN round-trip (a fire actually drives the game)
#
# Run by a user systemd timer (tools/scenario/systemd/); see the unit files. On a genuine failure it
# bonks Andrew, couriers spritz and leaves a non-zero record in $STATUS_DIR. It never fails silently.
#
# The timer runs it from a worktree dedicated to it (the unit's WorkingDirectory), and the first thing
# it does is fast-forward that worktree to origin/main and re-exec itself from the updated file, so the
# timer never runs a dev branch or a stale checkout. A tree it cannot fast-forward (dirty, diverged,
# detached, no route to origin) fails loudly with the alarm; the smoke never runs on stale code.
# The one-time setup of that worktree is in tools/scenario/systemd/README.md.
#
# The record ($STATUS_DIR/last.json and the run's result.json) carries the load average at the run's
# start and the timestamp of every NetGame state change; the in-game wait is 600 s (smoke.yaml). A pass
# that took more than SLOW_SECONDS to reach in game is still a PASS, flagged "slow" in last.json.
#
# Exit codes: 0 = the three checks passed, 1 = the client ran and a check FAILED (loud), 2 = the run
# could not start (the game-run lock was busy: another launch/verify run in progress) -- SKIPPED, not
# a failure. A busy lock is contention, not a harness break, so it never fires the alarm.
#
#   smoke.sh [--no-sync] [--sync-only] [--no-build] [--dry-run-alarm] [--alarm-dry-run REASON]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
SMOKE_YAML="$REPO/tools/scenario/smoke.yaml"
RUN_SCENARIO="$REPO/tools/scenario/run_scenario.py"
DLL="$REPO/build/mingw-scenario/bin/BugSplat64.dll"
STATUS_DIR="${NEVR_SMOKE_STATUS_DIR:-/var/tmp/work-nevr-runtime/scenario-smoke}"
RUN_DIR="$STATUS_DIR/run"
ALARM_FROM="scenario-smoke"
GAME_RUN_LOCK="${NEVR_LAUNCH_LOCK:-/var/tmp/work-nevr-runtime/launch-client.lock}"
SLOW_SECONDS=300

ORIG_ARGS=("$@")
SYNC=1
SYNC_ONLY=0
SYNCED=0
BUILD=1
DRY_RUN_ALARM=0
ALARM_DRY_RUN_REASON=""
USAGE="smoke.sh [--no-sync] [--sync-only] [--no-build] [--dry-run-alarm] [--alarm-dry-run REASON]"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-sync) SYNC=0; shift ;;
    --sync-only) SYNC_ONLY=1; shift ;;
    --synced) SYNCED=1; shift ;;
    --no-build) BUILD=0; shift ;;
    --dry-run-alarm) DRY_RUN_ALARM=1; shift ;;
    --alarm-dry-run) ALARM_DRY_RUN_REASON="${2:?--alarm-dry-run needs a reason string}"; shift 2 ;;
    *) echo "unknown argument: $1 (usage: $USAGE)" >&2; exit 2 ;;
  esac
done

# The alarm: bonk Andrew (persistent desktop notification; best-effort on a headless timer) and
# courier spritz (file mailbox; delivered). A failed alarm is reported, never swallowed: the smoke
# already failed, and its non-zero record is the source of truth.
alarm() {
  local reason="$1"
  if ! bonk --from "$ALARM_FROM" "NEVR scenario smoke FAILED: $reason" 2>&1; then
    echo "ALARM: bonk Andrew failed (rc=$?); the failure is still recorded in $STATUS_DIR/last.txt" >&2
  fi
  if ! courier send --to spritz --from "$ALARM_FROM" --msg "NEVR scenario smoke FAILED: $reason" 2>&1; then
    echo "ALARM: courier to spritz failed (rc=$?); the failure is still recorded in $STATUS_DIR/last.txt" >&2
  fi
}

# --alarm-dry-run prints the exact alarm commands the failure path would run, and fires nothing. It
# proves the alarm wiring (bonk Andrew + courier spritz) without a game run.
if [[ -n "$ALARM_DRY_RUN_REASON" ]]; then
  echo "=== alarm dry-run (fires nothing) ==="
  echo "would bonk:    bonk --from $ALARM_FROM \"NEVR scenario smoke FAILED: $ALARM_DRY_RUN_REASON\""
  echo "would courier: courier send --to spritz --from $ALARM_FROM --msg \"NEVR scenario smoke FAILED: $ALARM_DRY_RUN_REASON\""
  echo "bonk resolves to: $(command -v bonk)"
  echo "courier resolves to: $(command -v courier)"
  exit 0
fi

mkdir -p "$STATUS_DIR"

# write_record VERDICT EXIT REASON: the durable one-line verdict ($STATUS_DIR/last.json, last.txt).
write_record() {
  python3 -I - "$STATUS_DIR/last.json" "$STATUS_DIR/last.txt" "$1" "$2" "$3" <<'PY'
import datetime, json, sys
last_json, last_txt, verdict, code, reason = sys.argv[1:6]
json.dump({"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
           "verdict": verdict, "passed": verdict == "PASS", "exit": int(code), "reason": reason},
          open(last_json, "w"), indent=2)
open(last_txt, "w").write(f"{verdict}: {reason} (exit {code})\n")
PY
}

# Stage 1: bring this worktree to origin/main, then run the rest of the script from the updated
# file. The re-exec matters: bash reads a script as it runs, and git replaces the file on update.
sync_failed() {
  echo "=== SMOKE FAIL: $1 ===" >&2
  write_record FAIL 1 "$1"
  if [[ $DRY_RUN_ALARM -eq 1 ]]; then
    echo "[dry-run] alarm would fire: bonk Andrew + courier spritz: NEVR scenario smoke FAILED: $1" >&2
  else
    alarm "$1"
  fi
  exit 1
}
if [[ $SYNC -eq 1 && $SYNCED -eq 0 ]]; then
  git -C "$REPO" fetch --quiet origin main || sync_failed "cannot fetch origin/main for $REPO; the smoke never runs on a stale tree"
  [[ -z "$(git -C "$REPO" status --porcelain)" ]] || sync_failed "$REPO has uncommitted changes; the smoke worktree must be clean (it tracks origin/main and nothing else)"
  git -C "$REPO" merge --quiet --ff-only origin/main || sync_failed "cannot fast-forward $REPO to origin/main (diverged or detached); the smoke never runs on a dev branch"
  echo "=== smoke: $REPO at $(git -C "$REPO" rev-parse --short=8 HEAD) (origin/main) ==="
  [[ $SYNC_ONLY -eq 1 ]] && exit 0
  exec "$REPO/tools/scenario/smoke.sh" --synced "${ORIG_ARGS[@]}"
fi
[[ $SYNC_ONLY -eq 1 ]] && exit 0

# A busy game-run lock is contention with another launch/verify run, not a harness break: wait for it
# (bounded), and skip rather than fire the alarm if it never frees.
wait_for_lock() {
  local waited=0
  while fuser -s "$GAME_RUN_LOCK" 2>/dev/null; do
    if (( waited >= ${SMOKE_LOCK_WAIT_SECONDS:-900} )); then
      write_record SKIP 2 "game-run lock busy"
      echo "=== SMOKE SKIP: game-run lock $GAME_RUN_LOCK still busy after ${SMOKE_LOCK_WAIT_SECONDS:-900} s ===" >&2
      exit 2
    fi
    sleep 5
    waited=$((waited + 5))
  done
}
wait_for_lock

if [[ $BUILD -eq 1 ]]; then
  # AGENTS.md "Builds": wait while two or more ninja run; never start a third build on a busy machine.
  waited=0
  while [[ $(pgrep -x ninja | wc -l) -ge 2 ]] && (( waited < ${SMOKE_NINJA_WAIT_SECONDS:-1800} )); do
    sleep 10
    waited=$((waited + 10))
  done
  echo "=== smoke: building the scenario DLL (just preset=mingw-scenario build) ==="
  # `just build` greps its own output and always exits 0, so the raw cmake build after it is the check:
  # a failed build must never run the smoke on a stale DLL.
  (cd "$REPO" && just worktree-setup && just preset=mingw-scenario build && cmake --build --preset mingw-scenario) \
    || { write_record FAIL 1 "the scenario build failed"; alarm "the scenario build failed"; exit 1; }
fi
[[ -f "$DLL" ]] || { echo "ERROR: $DLL does not exist after build" >&2; exit 2; }

echo "=== smoke: one client boot, $SMOKE_YAML ==="
set +e
python3 "$RUN_SCENARIO" "$SMOKE_YAML" --dll "$DLL" --out "$RUN_DIR"
rc=$?
set -e

# A run that could not even start (lock grabbed between the pre-check and launch, or a game already
# running) is SKIPPED, not failed: it tells us nothing about the harness.
lock_busy=0
launcher_out="$RUN_DIR/launch-client.out"
if [[ -f "$launcher_out" ]] && grep -qE 'another game run|already running' "$launcher_out"; then
  lock_busy=1
fi

# Extract the verdict into the durable record: the first failing step on a failure, and always the
# load average at the run's start, every state timestamp and the seconds to in game.
python3 -I - "$RUN_DIR/result.json" "$STATUS_DIR/last.json" "$STATUS_DIR/last.txt" "$rc" "$lock_busy" "$SLOW_SECONDS" <<'PY'
import datetime, json, os, sys
result_path, last_json, last_txt, rc, lock_busy, slow_seconds = sys.argv[1:7]
rc, lock_busy, slow_seconds = int(rc), lock_busy == "1", float(slow_seconds)
passed, reason, data = False, "", {}
if os.path.isfile(result_path):
    try:
        data = json.load(open(result_path))
        passed = bool(data.get("passed"))
        for step in data.get("steps", []):
            if step.get("result") == "FAIL":
                reason = f"{step.get('step')}: {step.get('detail', '')}"
                break
    except (OSError, ValueError):
        pass
in_game_seconds = next((s.get("seconds") for s in data.get("steps", [])
                        if "reached in game" in str(s.get("step", "")) and s.get("result") == "PASS"), None)
if lock_busy:
    verdict, reason, exit_code = "SKIP", "game-run lock busy (another launch/verify run in progress)", 2
elif rc == 0 and passed:
    verdict, exit_code = "PASS", 0
else:
    verdict, exit_code = "FAIL", 1
slow = verdict == "PASS" and in_game_seconds is not None and in_game_seconds > slow_seconds
record = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
          "verdict": verdict, "passed": passed, "exit": exit_code, "reason": reason,
          "loadavg_start": data.get("loadavg_start"), "in_game_seconds": in_game_seconds,
          "slow": slow, "states": data.get("states", [])}
json.dump(record, open(last_json, "w"), indent=2)
note = f"; SLOW: in game after {in_game_seconds} s (> {slow_seconds:g} s)" if slow else ""
open(last_txt, "w").write(f"{verdict}: {reason or 'all three checks passed'}{note} (exit {exit_code})\n")
print(record["verdict"], exit_code, reason, sep="\t", flush=True)
PY

if [[ "$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("verdict",""))' "$STATUS_DIR/last.json")" == "SKIP" ]]; then
  echo "=== SMOKE SKIP: game-run lock busy (another launch/verify run in progress) ===" >&2
  exit 2
fi

if [[ $rc -eq 0 ]]; then
  echo "=== SMOKE PASS: all three checks passed ($RUN_DIR) ==="
  exit 0
fi

reason="$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("reason",""))' "$STATUS_DIR/last.json")"
echo "=== SMOKE FAIL: ${reason:-unknown} (exit $rc) ===" >&2

if [[ $DRY_RUN_ALARM -eq 1 ]]; then
  echo "[dry-run] alarm would fire: bonk Andrew + courier spritz: NEVR scenario smoke FAILED: $reason" >&2
  exit 1
fi
alarm "$reason"
exit 1
