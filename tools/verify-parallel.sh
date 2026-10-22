#!/usr/bin/env bash
# Run independent `just` test recipes concurrently, capped at nproc workers, fail-closed
# and named. Used by `just verify` so its eight test suites run side by side instead of
# one after another (each test-quest-* compiles its own host binary and runs it, so the
# serial wall clock was ~8 compiles back to back).
#
#   verify-parallel.sh <recipe> [<recipe> ...]
#
# Each recipe is run as `just <recipe>` with stdout+stderr captured to
# build/verify-parallel/<recipe>.log. Live START/PASS/FAIL lines are printed as workers
# start and finish; the full logs are printed in argument order at the end so the pass
# evidence is readable and a pass is never re-run to prove it.
#
# Fail-closed: a worker's exit status is the exit status of `just <recipe>` (not a
# scheduler proxy like xargs' 123). Any nonzero status fails the whole block, and the
# summary names every failed suite.
#
# Core cap / no oversubscription: at most nproc workers run at once. Every suite but
# test-auth-unit compiles one translation unit at a time with a single-threaded g++.
# test-auth-unit is the only recipe that drives cmake/ninja, which would otherwise grab
# every core itself (a bare ninja -j default plus the other workers = thrash, and could
# OOM a 16G runner). CMAKE_BUILD_PARALLEL_LEVEL is pinned to its fair share so the total
# stays at nproc.
set -euo pipefail

suites=("$@")
if ((${#suites[@]} == 0)); then
    echo "verify-parallel: no suite recipes given" >&2
    exit 2
fi

# Results are keyed by suite name, so a repeated name would let a pass overwrite a failure.
declare -A seen_suites=()
for suite in "${suites[@]}"; do
    if [[ -n "${seen_suites[$suite]:-}" ]]; then
        echo "verify-parallel: duplicate suite recipe: $suite" >&2
        exit 2
    fi
    seen_suites["$suite"]=1
done

root="$(git rev-parse --show-toplevel)"
cd "$root"

nproc="$(nproc)"
jobs_max="$nproc"
((${#suites[@]} < jobs_max)) && jobs_max=${#suites[@]}

out="build/verify-parallel"
mkdir -p "$out"

# jobs_max workers run at once; jobs_max-1 of them use one core each (single g++), so the
# ninja worker gets the remaining cores. Floored at 1 so total <= nproc.
share=$((nproc - jobs_max + 1))
((share < 1)) && share=1
export CMAKE_BUILD_PARALLEL_LEVEL="$share"

declare -A running    # pid -> recipe name
declare -A statuses   # recipe name -> exit code

run_one() {
    local suite="$1" t0 rc
    local log="$out/$suite.log"
    t0=$SECONDS
    printf '[%s] START\n' "$suite"
    if just "$suite" >"$log" 2>&1; then
        rc=0
    else
        rc=$?
    fi
    if ((rc == 0)); then
        printf '[%s] PASS (%ds)\n' "$suite" "$((SECONDS - t0))"
    else
        printf '[%s] FAIL (exit %d, %ds)\n' "$suite" "$rc" "$((SECONDS - t0))"
    fi
    return "$rc"
}

launched=0
nrunning=0
while ((launched < ${#suites[@]} || nrunning > 0)); do
    # Launch workers while slots are free.
    while ((launched < ${#suites[@]} && nrunning < jobs_max)); do
        suite="${suites[$launched]}"
        run_one "$suite" &
        running[$!]="$suite"
        launched=$((launched + 1))
        nrunning=$((nrunning + 1))
    done
    # Reap exactly one finished worker. `wait -n` returns that job's own exit status; the
    # `|| rc=$?` keeps set -e from aborting the scheduler on a failing suite.
    done_pid=""
    rc=0
    wait -n -p done_pid || rc=$?
    suite="${running[$done_pid]}"
    unset 'running[$done_pid]'
    statuses["$suite"]=$rc
    nrunning=$((nrunning - 1))
done

failed=0
failed_names=()
for suite in "${suites[@]}"; do
    rc="${statuses[$suite]}"
    printf '===== [%s] exit %s =====\n' "$suite" "$rc"
    if [[ -f "$out/$suite.log" ]]; then
        cat "$out/$suite.log"
    else
        echo "(no log written for $suite)"
    fi
    echo
    if ((rc != 0)); then
        failed=1
        failed_names+=("$suite")
    fi
done

if ((failed)); then
    printf 'verify: FAIL — %d of %d suite(s) failed: %s\n' "${#failed_names[@]}" "${#suites[@]}" "${failed_names[*]}" >&2
    exit 1
fi
printf 'verify: all %d suites passed (jobs=%d, nproc=%d)\n' "${#suites[@]}" "$jobs_max" "$nproc"
