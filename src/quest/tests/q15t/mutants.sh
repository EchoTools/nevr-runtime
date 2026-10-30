#!/usr/bin/env bash
# Each mutant removes one rule from a scratch copy of src/; the host tests must FAIL on it. A surviving mutant is a missing test.
set -uo pipefail
# Every build and test runs in the shared capped slice: re-exec inside it when not there already.
if ! grep -q 'nevr-builds.slice' /proc/self/cgroup 2>/dev/null; then
  exec systemd-run --user --scope --quiet --slice=nevr-builds.slice -p MemoryMax=4G -- bash "$0" "$@"
fi
# one mutant run at a time on this machine, across every seat and worker: a second run waits here
exec 9>"${XDG_RUNTIME_DIR:-/tmp}/q15t-mutants.lock"
flock 9
# each mutant in its own capped scope: a runaway is killed alone and scores as killed
cap=(systemd-run --user --scope --quiet --slice=nevr-builds.slice -p MemoryMax=4G --)
tests="$(cd "$(dirname "$0")" && pwd)"
here="${Q15T_PLUGIN_DIR:-$(cd "$tests/../../plugins/q15t" && pwd)}"
# A mutant runs in a scratch copy, so the checkout root (where the SDK headers are) is told to the test runner.
export Q15T_NEVR_ROOT="$(cd "$here/../../../.." && pwd)"
# one scratch tree per run: two runs at once never share a mutant directory
scratch="$Q15T_NEVR_ROOT/build/q15t-mutants/run-$$"
mkdir -p "$scratch"
trap 'rm -rf -- "${scratch:?}"' EXIT
# Q15T_MUTANTS_ONLY=<module>: only that module's tests/mutants.d/<module>.sh (the core list is skipped); =core: only the core list below
# Q15T_MUTANTS_NAME=<regex>: only the mutants whose name matches (e.g. '^(no_build_check|tick_no_vptr)$' with Q15T_MUTANTS_ONLY=core)
only="${Q15T_MUTANTS_ONLY:-}"
wanted() { [ -z "${Q15T_MUTANTS_NAME:-}" ] || [[ "$1" =~ $Q15T_MUTANTS_NAME ]]; }
survived=0
# runm <module> <name> <file> <sed>: like run, but only that module's tests (tests/mod/<module>.sh) judge the mutant
runm() {
  wanted "$2" || return 0
  rm -rf -- "${scratch:?}/${2:?}" && mkdir -p "$scratch/$2" && cp -r "$here/src" "$tests" "$scratch/$2/"
  sed -i "$4" "$scratch/$2/$3"
  if cmp -s "$here/$3" "$scratch/$2/$3"; then echo "MUTANT $2: sed changed nothing"; survived=$((survived+1)); return; fi
  if (cd "$scratch/$2" && Q15T_PLUGIN_DIR="$scratch/$2" Q15T_MODULE="$1" Q15T_TEST_OUT="$scratch/$2/out" "${cap[@]}" bash q15t/run_host_tests.sh >/dev/null 2>&1); then echo "MUTANT $2: SURVIVED"; survived=$((survived+1)); else echo "MUTANT $2: killed"; fi
}
run() { # name file sed-expression
  [ -n "$only" ] && [ "$only" != core ] && return 0
  wanted "$1" || return 0
  rm -rf -- "${scratch:?}/${1:?}" && mkdir -p "$scratch/$1" && cp -r "$here/src" "$tests" "$scratch/$1/"
  sed -i "$3" "$scratch/$1/$2"
  if cmp -s "$here/$2" "$scratch/$1/$2"; then echo "MUTANT $1: sed changed nothing"; survived=$((survived+1)); return; fi
  if (cd "$scratch/$1" && Q15T_PLUGIN_DIR="$scratch/$1" "${cap[@]}" bash q15t/run_host_tests.sh >/dev/null 2>&1); then echo "MUTANT $1: SURVIVED"; survived=$((survived+1)); else echo "MUTANT $1: killed"; fi
}
run no_exec_check   src/got_swap.cpp     "s/if (m.perms\[2\] == 'x') {/if (false) {/"
# the pre-check and the compare-and-swap are two guards of one rule: drop both, or the CAS alone still holds it
run no_expected     src/got_swap.cpp     "s/if (r.original != expected) {/if (false) {/;s/const bool swapped = word->compare_exchange_strong(want, replacement, std::memory_order_acq_rel);/word->store(replacement); const bool swapped = true; (void)want;/"
run no_restore      src/got_swap.cpp     "s/mprotect(reinterpret_cast<void\*>(base), page, PROT_READ) != 0/false/"
run no_build_check  src/plugin.cpp       "s/|| std::strcmp(id, Q15T_BUILD_ID) != 0//"
run y_never_raises  src/clock_levels.cpp "s/send_cpu = Raised(cpu, Q15T_CPU_LEVEL);/send_cpu = cpu;/"
run y_lowers        src/clock_levels.cpp "s/return asked < floor ? floor : asked;/return floor;/"
run x_raises        src/clock_levels.cpp "s/std::int32_t send_cpu = cpu, send_gpu = gpu;/std::int32_t send_cpu = 4, send_gpu = 4;/"
# the clock switch (args {clock: raise}), one binary for both arms
run raise_always    src/clock_levels.cpp "s/const bool raise = g_raise.load(std::memory_order_relaxed);/const bool raise = true;/"
run raise_never     src/clock_levels.cpp "s/const bool raise = g_raise.load(std::memory_order_relaxed);/const bool raise = false;/"
run raise_log_wrong src/clock_levels.cpp "s/{\"candidate\", raise ? 1 : 0}/{\"candidate\", 0}/"
run init_no_set     src/plugin.cpp       "s/^  SetClockRaise(raise);$//"
run init_raise_any  src/plugin.cpp       "s/const bool raise = has_clock_arg \&\& std::strcmp(clock_arg, \"raise\") == 0;/const bool raise = has_clock_arg;/"
run init_raise_pfx  src/plugin.cpp       "s/std::strcmp(clock_arg, \"raise\") == 0;/std::strncmp(clock_arg, \"raise\", 5) == 0;/"
run init_no_warn    src/plugin.cpp       "s/if (has_clock_arg \&\& !raise \&\& std::strcmp(clock_arg, \"log\") != 0) {/if (false) {/"
run init_warn_log   src/plugin.cpp       "s/ \&\& std::strcmp(clock_arg, \"log\") != 0) {/) {/"
run init_arg_unlogged src/plugin.cpp     "s/{\"clock_arg\", clock_arg}/{\"clock_arg\", \"\"}/"
run args_key_prefix src/plugin_args.cpp  "s/std::strcmp(k, key) == 0;/std::strncmp(k, key, std::strlen(key)) == 0;/"
run args_any_key    src/plugin_args.cpp  "s/const bool match = std::strcmp(k, key) == 0;/const bool match = true;/"
run args_value_cut  src/plugin_args.cpp  "s/      if (!vfits) {/      if (false) {/"
run args_no_comma   src/plugin_args.cpp  "s/    if (args_json\[i\] == ',') {/    if (true) {/"
run args_no_colon   src/plugin_args.cpp  "s/    if (args_json\[i\] != ':') return false;/    if (false) return false;/"
run args_escape     src/plugin_args.cpp  "s#    if (c == '..') {#    if (false) {#"
run args_escape_dflt src/plugin_args.cpp "s#default: c = e; break;#default: c = '?'; break;#"
run hz_never_switch src/refresh_rate.cpp "s/const bool in = kind == HzLevelKind::kCombatOrLobby;/const bool in = false;/"
run hz_any_level    src/refresh_rate.cpp "s/if (lv == c) {/if (lv != 0) {/"
run hz_req_ignored  src/refresh_rate.cpp "s/if (g_rule.load(std::memory_order_acquire) \&\& hz > low \&\& g_in_match.load(std::memory_order_acquire)) {/if (false) {/"
run hz_no_lobby     src/refresh_rate.cpp "s/(lobby \&\& In(lv, kLobbyLevels))/(false)/"
run hz_arena_loses  src/refresh_rate.cpp "s/if (lobby \&\& In(lv, kEchoArenaLevels)) {/if (false) {/"
run hz_no_quest2    src/refresh_rate.cpp "s/r.quest2 = std::strcmp(device, \"hollywood\") == 0;/r.quest2 = false;/"
run hz_q2off_ignored src/refresh_rate.cpp "s/ \&\& !r.q2_off ? kQuest2Hz/ ? kQuest2Hz/"
run hz_leveloff_ignored src/refresh_rate.cpp "s/r.on = r.compiled \&\& !r.level_off;/r.on = r.compiled;/"
run tick_no_vptr    src/game_tick.cpp    "s/if (vptr != g_vptr.load(std::memory_order_relaxed) || !ok) {/if (!ok) {/"
run gate_everywhere src/headset_gates.cpp "s/if (Within(ret, g_party_lo, g_party_hi)) {/if (true) {/"
run gate_never      src/headset_gates.cpp "s/if (Within(ret, g_head_lo, g_head_hi)) {/if (false) {/"
run gate_link_never src/headset_gates.cpp "s/if (Within(ret, g_link_lo, g_link_hi)) {/if (false) {/"
run gate_link_everywhere src/headset_gates.cpp "s/if (Within(ret, g_link_lo, g_link_hi)) {/if (true) {/"
run gate_link_answer src/headset_gates.cpp "s/return g_link_answer.load(std::memory_order_relaxed);/return kNotQuest;/"
run gate_link_unset src/headset_gates.cpp "s/  g_link_answer.store(link_answer);//"
run gate_link_range_lo src/headset_gates.cpp "s/g_link_lo.store(base + link_range.lo_vaddr);/g_link_lo.store(base);/"
run gate_link_range_hi src/headset_gates.cpp "s/g_link_hi.store(base + link_range.hi_vaddr);/g_link_hi.store(~static_cast<std::uintptr_t>(0));/"
run gate_link_ctr src/headset_gates.cpp "s/g_link_gated.fetch_add(1, std::memory_order_relaxed) + 1;/g_link_gated.load() + 1;/"
run gate_link_wire_off src/plugin.cpp "s/Q15T_GATE_LINK ? CallerRange{Q15T_LINK_FN_LO, Q15T_LINK_FN_HI} : CallerRange{0, 0}/CallerRange{0, 0}/"
run gate_link_wire_bots src/plugin.cpp "s/gates.bots ? CallerRange{Q15T_BOTS_FN_LO, Q15T_BOTS_FN_HI} : CallerRange{0, 0}/CallerRange{Q15T_BOTS_FN_LO, Q15T_BOTS_FN_HI}/"
run gate_bad_range  src/headset_gates.cpp "s/return addr - lo.load(std::memory_order_relaxed) < hi.load(std::memory_order_relaxed) - lo.load(std::memory_order_relaxed);/return addr >= lo.load(std::memory_order_relaxed);/"
# T-26
run t26_no_hold        src/weapon_arm.cpp "s/if (++g_hold < kHold) return;/++g_hold;/"
run t26_no_marker      src/weapon_arm.cpp "s/if (access(g_marker, F_OK) == 0) {/if (false) {/"
run t26_ignore_right   src/weapon_arm.cpp "s/if (arm == 1 || prof_arm == 1) {/if (false) {/"
run t26_ignore_profile src/weapon_arm.cpp "s/if (arm == 1 || prof_arm == 1) {/if (arm == 1) {/"
run t26_queue_arg      src/weapon_arm.cpp "s/\*reinterpret_cast<std::uint64_t\*>(q + 0x20) = 1;/*reinterpret_cast<std::uint64_t*>(q + 0x20) = 0;/"
run t26_always_direct  src/weapon_arm.cpp "s/if (direct != 0) {/if (true) {/"
run t26_no_giveup      src/weapon_arm.cpp "s/if (now - g_set_at > kVerifyS) {/if (false) {/"
run t26_marker_early   src/weapon_arm.cpp "s/    if (arm == 1) {/    if (true) {/"
run t26_no_vptr        src/gamespace.cpp  "s/ || Rd<std::uintptr_t>(cs, \&ok3) != vptr_abs//"
run t26_no_key         src/gamespace.cpp  "s/if (Rd<std::uint64_t>(sys + j \* 16, \&ok3) != key || !ok3) continue;/if (!ok3) continue;/"
run t26_no_gs_link     src/gamespace.cpp  "s/ || Rd<std::uintptr_t>(cs + 0x80, \&ok3) != gs//"
run t26_no_valid       src/weapon_arm.cpp "s/if (ok \&\& g_f.is_valid(reinterpret_cast<void\*>(found\[i\].cs), h)) {/if (ok) {/"
run t26_off_ignored    src/weapon_arm.cpp "s/if (access(off, F_OK) == 0) {/if (false) {/"
run t26_dir_unchecked  src/weapon_arm.cpp "s/    return access(out, W_OK) == 0;\n  }/    return true;\n  }/;0,/return access(out, W_OK) == 0;/s//return true;/"
run t26_repeats        src/weapon_arm.cpp "s/  if (g_done) return;\n  Step/  Step/;s/^  if (g_done) return;$//"
run t26_quiet_missing  src/weapon_arm.cpp "s/LogT26(NEVR_LOG_ERROR, \"t26 off\"/LogT26(NEVR_LOG_INFO, \"t26 off\"/"
# module mutants: tests/mutants.d/<module>.sh, each a list of `runm <module> <name> <file> <sed>` lines
for f in "$tests"/mutants.d/*.sh; do
  [ -e "$f" ] || continue
  [ -n "$only" ] && [ "$(basename "$f" .sh)" != "$only" ] && continue
  . "$f"
done
exit $survived
