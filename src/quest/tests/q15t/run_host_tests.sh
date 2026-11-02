#!/usr/bin/env bash
# Host tests for both arms. usage: tests/run_host_tests.sh [mutant-define]   (e.g. -DQ15T_MUTANT_NO_EXEC_CHECK is applied by tools/mutants.sh)
set -euo pipefail
# Every build and test runs in the shared capped slice: re-exec inside it when not there already.
if ! grep -q 'nevr-builds.slice' /proc/self/cgroup 2>/dev/null; then
  exec systemd-run --user --scope --quiet --slice=nevr-builds.slice -p MemoryMax=4G -- bash "$0" "$@"
fi
# Memory cap for every compile and test process this script starts (and so for every mutant): a test that allocates forever
# dies alone (bad_alloc / SIGSEGV, scored as killed) instead of pushing the desktop into the OOM killer. 2 GiB of address space.
ulimit -v "${Q15T_TEST_VMEM_KB:-2097152}"
tests="$(cd "$(dirname "$0")" && pwd)"
here="${Q15T_PLUGIN_DIR:-$(cd "$tests/../../plugins/q15t" && pwd)}"
# The checkout root: the SDK headers are $nevr/sdk/include/nevr/. This directory is src/quest/plugins/q15t.
nevr="${Q15T_NEVR_ROOT:-$(cd "$here/../../../.." && pwd)}"
out="${Q15T_TEST_OUT:-$nevr/build/q15t-host-tests}"
mkdir -p "$out"
# Q15T_MODULE=<name>: run only tests/mod/<name>.sh (used by the module mutants); else everything
if [ -n "${Q15T_MODULE:-}" ]; then
  . "$tests/mod/${Q15T_MODULE}.sh"
  exit 0
fi
for arm in "0 0" "1 0" "0 1"; do
  set -- $arm; cap=$1; t26=$2; shift 2 || true
  g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -rdynamic -Wall -Wextra -Werror \
    -DQ15T_HZ_MATCH90=$cap -DQ15T_T26=$t26 -DQ15T_SLOT_SETCLOCKLEVELS=0x10 -DQ15T_SLOT_SETDISPLAYREFRESHRATE=0x18 -include "$tests/host_overrides.h" \
    -DQ15T_BUILD_ID_FOR_TEST='"b243509c08ce677aeb95fa348016949b3fc45230"' "${@}" \
    -I"$here/src" -I"$nevr/sdk/include" \
    "$tests/host_test.cpp" "$here/src/plugin.cpp" "$here/src/plugin_log.cpp" "$here/src/script_redirect.cpp" "$here/src/import_hook.cpp" "$here/src/clock_levels.cpp" "$here/src/plugin_args.cpp" "$here/src/refresh_rate.cpp" "$here/src/game_tick.cpp" "$here/src/safe_read.cpp" "$here/src/headset_gates.cpp" "$here/src/got_swap.cpp" "$here/src/weapon_arm.cpp" "$here/src/gamespace.cpp" \
    -o "$out/host_test_${cap}_${t26}" -ldl
  "$out/host_test_${cap}_${t26}"
done

# the headset gates (return-address gated wrappers)
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/gates_test.cpp" "$here/src/headset_gates.cpp" "$here/src/plugin_log.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" \
  -o "$out/gates_test" -ldl
"$out/gates_test"

# modules: one tests/mod/<name>.sh each (sourced with $here $nevr $out set; it compiles and runs its own test, and fails loudly)
for m in "$tests"/mod/*.sh; do
  [ -e "$m" ] || continue
  echo "module: $(basename "$m" .sh)"
  . "$m"
done
