#!/usr/bin/env bash
# Each mutant removes one rule from a scratch copy of src/; the host tests must FAIL on it. A surviving mutant is a missing test.
# The scratch copy is configured as its own build of this CMake project (the preset q15t-host-tests, with Q15T_PLUGIN_DIR pointing at the
# copy), the module's target is built and its ctest label run; a compile error or a failed check kills the mutant. The arm64 checks
# (preset q15t-ndk-checks) are not part of the verdict: they judge a build, not a rule.
set -uo pipefail
# Every build and test runs in the shared capped slice: re-exec inside it when not there already.
if ! grep -q 'nevr-builds.slice' /proc/self/cgroup 2>/dev/null; then
  exec systemd-run --user --scope --quiet --slice=nevr-builds.slice -p MemoryMax=4G -- bash "$0" "$@"
fi
# one mutant run at a time on this machine, across every seat and worker: a second run waits here
exec 9>"${XDG_RUNTIME_DIR:-/tmp}/q15t-mutants.lock"
flock 9
# each mutant's tests run in their own capped scope: a runaway is killed alone and scores as killed
cap=(systemd-run --user --scope --quiet --slice=nevr-builds.slice -p MemoryMax=4G --)
tests="$(cd "$(dirname "$0")" && pwd)"
here="${Q15T_PLUGIN_DIR:-$(cd "$tests/../../plugins/q15t" && pwd)}"
# A mutant runs in a scratch copy, so the checkout root (where the SDK headers are) is told to the CMake project.
Q15T_NEVR_ROOT="$(cd "$here/../../../.." && pwd)"
# one scratch tree per run: two runs at once never share a mutant directory
scratch="$Q15T_NEVR_ROOT/build/q15t-mutants/run-$$"
mkdir -p "$scratch"
trap 'rm -rf -- "${scratch:?}"' EXIT
# Q15T_MUTANTS_ONLY=<module>: only that module's tests/mutants.d/<module>.sh (the core list is skipped); =core: only the core list below
# Q15T_MUTANTS_NAME=<regex>: only the mutants whose name matches (e.g. '^(no_build_check|tick_no_vptr)$' with Q15T_MUTANTS_ONLY=core)
only="${Q15T_MUTANTS_ONLY:-}"
wanted() { [ -z "${Q15T_MUTANTS_NAME:-}" ] || [[ "$1" =~ $Q15T_MUTANTS_NAME ]]; }
survived=0
# What a per-thread word becomes in a "shared across threads" mutant: one value for every thread, same interface as q15t::ThreadWord.
SHARED_WORD='struct SharedWord { bool Create() { return true; } std::uintptr_t Get() const { return v; } void Set(std::uintptr_t x) { v = x; } std::uintptr_t v = 0; }'
# judge <dir> <module>: configure, build and run the host tests of the scratch copy <dir> ({src,q15t}); with a module, only that module's.
# Succeeds when every test passes, so a SURVIVED mutant is a success here. A runaway dies alone under the address-space cap.
judge() {
  local dir="$1" module="${2:-}"
  local build_args=() ctest_args=()
  if [ -n "$module" ]; then
    build_args=(--target "mod_${module}")
    ctest_args=(-L "^${module}\$")
  fi
  (cd "$dir/q15t" \
    && cmake --preset q15t-host-tests -B "$dir/build" -DQ15T_PLUGIN_DIR="$dir" -DQ15T_SDK_INCLUDE="$Q15T_NEVR_ROOT/sdk/include" \
    && cmake --build "$dir/build" "${build_args[@]}" \
    && "${cap[@]}" bash -c 'ulimit -v "${Q15T_TEST_VMEM_KB:-2097152}" && exec ctest --test-dir "$0" "${@:1}"' "$dir/build" "${ctest_args[@]}")
}
# runm <module> <name> <file> <sed>: like run, but only that module's tests (tests/mod/<module>.cmake) judge the mutant
runm() {
  wanted "$2" || return 0
  rm -rf -- "${scratch:?}/${2:?}" && mkdir -p "$scratch/$2" && cp -r "$here/src" "$scratch/$2/" && cp -r "$tests" "$scratch/$2/q15t"
  sed -i "$4" "$scratch/$2/$3"
  if cmp -s "$here/$3" "$scratch/$2/$3"; then echo "MUTANT $2: sed changed nothing"; survived=$((survived+1)); rm -rf -- "${scratch:?}/${2:?}"; return; fi
  if judge "$scratch/$2" "$1" >/dev/null 2>&1; then echo "MUTANT $2: SURVIVED"; survived=$((survived+1)); else echo "MUTANT $2: killed"; fi
  rm -rf -- "${scratch:?}/${2:?}"
}
run() { # name file sed-expression
  [ -n "$only" ] && [ "$only" != core ] && return 0
  wanted "$1" || return 0
  rm -rf -- "${scratch:?}/${1:?}" && mkdir -p "$scratch/$1" && cp -r "$here/src" "$scratch/$1/" && cp -r "$tests" "$scratch/$1/q15t"
  sed -i "$3" "$scratch/$1/$2"
  if cmp -s "$here/$2" "$scratch/$1/$2"; then echo "MUTANT $1: sed changed nothing"; survived=$((survived+1)); rm -rf -- "${scratch:?}/${1:?}"; return; fi
  if judge "$scratch/$1" >/dev/null 2>&1; then echo "MUTANT $1: SURVIVED"; survived=$((survived+1)); else echo "MUTANT $1: killed"; fi
  rm -rf -- "${scratch:?}/${1:?}"
}
run no_exec_check   src/got_swap.cpp     "s/if (m.perms\[2\] == 'x') {/if (false) {/"
# the pre-check and the compare-and-swap are two guards of one rule: drop both, or the CAS alone still holds it
run no_expected     src/got_swap.cpp     "s/if (r.original != expected) {/if (false) {/;s/const bool swapped = word->compare_exchange_strong(want, replacement, std::memory_order_acq_rel);/word->store(replacement); const bool swapped = true; (void)want;/"
run no_restore      src/got_swap.cpp     "s/mprotect(reinterpret_cast<void\*>(base), page, PROT_READ) != 0/false/"
run no_build_check  src/plugin.cpp       "s/|| std::strcmp(id, Q15T_BUILD_ID) != 0//"
run tick_no_vptr    src/game_tick.cpp    "s/if (vptr != g_vptr.load(std::memory_order_relaxed) || !ok) {/if (!ok) {/"
# module mutants: tests/mutants.d/<module>.sh, each a list of `runm <module> <name> <file> <sed>` lines
for f in "$tests"/mutants.d/*.sh; do
  [ -e "$f" ] || continue
  [ -n "$only" ] && [ "$(basename "$f" .sh)" != "$only" ] && continue
  . "$f"
done
exit $survived
