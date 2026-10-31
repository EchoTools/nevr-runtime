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
# On a genuine failure it prints the reason on standard error, exits non-zero and leaves a non-zero
# record in $STATUS_DIR. It never fails silently.
#
# A scheduler runs it from a worktree dedicated to it (the current directory's checkout), and the first
# thing it does is fast-forward that worktree to origin/main and re-exec itself from the updated file,
# so a scheduled run never uses a dev branch or a stale checkout. A tree it cannot bring to exactly
# origin/main on its own branch (dirty, diverged, ahead, detached, another branch, no route to origin
# within the fetch timeout) fails loudly; the smoke never runs on stale code.
#
# The record ($STATUS_DIR/last.json and the run's result.json) carries the load average at the run's
# start and the timestamp of every NetGame state change; the in-game wait is 600 s (smoke.yaml). A pass
# that took more than SLOW_SECONDS to reach in game is still a PASS, flagged "slow" in last.json.
#
# Exit codes: 0 = the three checks passed, 1 = the client ran and a check FAILED (loud), 2 = the run
# could not start (the game-run lock was busy: another launch/verify run in progress) -- SKIPPED, not
# a failure. A busy lock is contention, not a harness break, so it is recorded as SKIP.
#
#   smoke.sh [--no-sync] [--sync-only] [--no-build]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
SMOKE_YAML="$REPO/tools/scenario/smoke.yaml"
RUN_SCENARIO="$REPO/tools/scenario/run_scenario.py"
DLL="$REPO/build/mingw-scenario/bin/BugSplat64.dll"
# NEVR_WORK_ROOT and work_dir (tools/lib/work_root.sh).
# shellcheck source=tools/lib/work_root.sh
source "$REPO/tools/lib/work_root.sh"
STATUS_DIR="${NEVR_SMOKE_STATUS_DIR:-$NEVR_WORK_ROOT/scenario-smoke}"
RUN_DIR="$STATUS_DIR/run"
GAME_RUN_LOCK="${NEVR_LAUNCH_LOCK:-$NEVR_WORK_ROOT/launch-client.lock}"
SLOW_SECONDS=300
# The branch this worktree exists to track origin/main on; any other state is a dev tree.
SMOKE_BRANCH="${NEVR_SMOKE_BRANCH:-nevr-runtime/792-smoke-main}"

ORIG_ARGS=("$@")
SYNC=1
SYNC_ONLY=0
SYNCED=0
BUILD=1
USAGE="smoke.sh [--no-sync] [--sync-only] [--no-build]"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-sync) SYNC=0; shift ;;
    --sync-only) SYNC_ONLY=1; shift ;;
    --synced) SYNCED=1; shift ;;
    --no-build) BUILD=0; shift ;;
    *) echo "unknown argument: $1 (usage: $USAGE)" >&2; exit 2 ;;
  esac
done

work_dir "$STATUS_DIR" "The daily scenario smoke (tools/scenario/smoke.sh): the last verdict, last.json and last.txt, and the run's own files." \
  "kept: each run overwrites the record and the files of the run directory."

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
  exit 1
}
if [[ $SYNC -eq 1 && $SYNCED -eq 0 ]]; then
  # No prompt and a bounded wait: an unreachable or hung origin is a loud failure, not a hung timer.
  GIT_TERMINAL_PROMPT=0 timeout "${SMOKE_FETCH_TIMEOUT_SECONDS:-120}" git -C "$REPO" fetch --quiet origin main \
    || sync_failed "cannot fetch origin/main for $REPO within ${SMOKE_FETCH_TIMEOUT_SECONDS:-120} s; the smoke never runs on a stale tree"
  branch="$(git -C "$REPO" symbolic-ref --short -q HEAD || true)"
  [[ "$branch" == "$SMOKE_BRANCH" ]] || sync_failed "$REPO is on ${branch:-a detached HEAD}, not $SMOKE_BRANCH; the smoke runs only from the worktree that tracks origin/main"
  [[ -z "$(git -C "$REPO" status --porcelain)" ]] || sync_failed "$REPO has uncommitted changes; the smoke worktree must be clean (it tracks origin/main and nothing else)"
  git -C "$REPO" merge --quiet --ff-only origin/main || sync_failed "cannot fast-forward $REPO to origin/main (diverged); the smoke never runs on a dev branch"
  # A fast-forward succeeds when HEAD is already ahead of origin/main; that is a dev commit.
  [[ "$(git -C "$REPO" rev-parse HEAD)" == "$(git -C "$REPO" rev-parse origin/main)" ]] \
    || sync_failed "$REPO is not at origin/main after the fast-forward (it carries commits of its own); the smoke never runs on a dev commit"
  echo "=== smoke: $REPO at $(git -C "$REPO" rev-parse --short=8 HEAD) (origin/main) ==="
  [[ $SYNC_ONLY -eq 1 ]] && exit 0
  exec "$REPO/tools/scenario/smoke.sh" --synced "${ORIG_ARGS[@]}"
fi
[[ $SYNC_ONLY -eq 1 ]] && exit 0

# A busy game-run lock is contention with another launch/verify run, not a harness break: wait for it
# (bounded), and skip rather than fail if it never frees.
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
  # `just build` fails on a failed build, so a broken build never runs the smoke on a stale DLL.
  (cd "$REPO" && just worktree-setup && just preset=mingw-scenario build) \
    || { write_record FAIL 1 "the scenario build failed"; echo "=== SMOKE FAIL: the scenario build failed ===" >&2; exit 1; }
fi
[[ -f "$DLL" ]] || { echo "ERROR: $DLL does not exist after build" >&2; exit 2; }
[[ -f "$(dirname "$DLL")/nevr-runtime.dll" ]] || { echo "ERROR: $(dirname "$DLL")/nevr-runtime.dll does not exist after build" >&2; exit 2; }

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
exit 1
