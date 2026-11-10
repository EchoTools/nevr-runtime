# ecfx: each rule of src/quest/combat/ecfx.cpp and of the drain rules it shares with PC, src/runtime/patch/ecfx_rules.h (#2349), removed or
# bent once; src/quest/tests/ecfx_test.cpp must fail on every one (the rules' mutants are also killed by PC's test_ecfx_rules).
# Sourced by ../mutants.sh. The run judges ecfx_test (the complete wall library); ecfx_test_missing is the same program on the other library.
C=src/runtime/patch/ecfx_rules.h
F=src/quest/combat/ecfx.cpp
runm ecfx_stale        $C "s#const bool stale = last \&\& nowMs - last > kStaleMs;#const bool stale = false;#"
runm ecfx_stale_edge   $C "s#nowMs - last > kStaleMs#nowMs - last >= kStaleMs#"
runm ecfx_stale_first  $C "s#const bool stale = last \&\& #const bool stale = #"
runm ecfx_one_per_ctx  $C "s#if (done) continue;#(void)done;#"
runm ecfx_lowest_kind  $C "s#(wj \& 3) < (Word(q, best) \& 3)#(wj \& 3) > (Word(q, best) \& 3)#"
runm ecfx_complete_alt $C "s#if (wj \&\& (wj#if ((wj#"
runm ecfx_boom         $C "s#if (kind == kBoom) {#if (false) {#"
runm ecfx_entry_exact  $C "s#(env.entries \&\& fn != env.entries->entry\[kind\])#false#"
runm ecfx_flag_bits    $C "s#if (!fn || (wb \& 0xc) || #if (!fn || #"
runm ecfx_nonzero_fn   $C "s#if (!fn || (wb \& 0xc) || #if ((wb \& 0xc) || #"
runm ecfx_inflight     $C "s#if (!w) {#if (!w \&\& false) {#"
runm ecfx_sound_again  $C "s#if (!wall || sounds.Again(wall, nowMs)) {#if (!wall) {#"
runm ecfx_sound_wall   $C "s#if (!wall || sounds.Again(wall, nowMs)) {#if (sounds.Again(wall, nowMs)) {#"
runm ecfx_sound_ms     $C "s#kSoundMs = 3000#kSoundMs = 2000#"
runm ecfx_sound_edge   $C "s#nowMs - at\[i\] < kSoundMs#nowMs - at[i] <= kSoundMs#"
runm ecfx_named        $F "s#if (Named(r.fn, r.kind)) {#if (true) {#"
runm ecfx_gate_never   $F "s#if (!g_gateOn.load(std::memory_order_relaxed)) {#if (false) {#"
runm ecfx_gate_early   $F "s#^  g_attached = true;#  g_attached = true; __atomic_store_n(Gate(), 1u, __ATOMIC_RELEASE);#"
runm ecfx_cadence      $F "s#if (g_lastMs \&\& now - g_lastMs < kEveryMs) return;##"
runm ecfx_runner       $F "s#if (g_running.test_and_set(std::memory_order_acquire)) return;##"
runm ecfx_tick_slot    $F "s#if (!ec::OnNetTick(\&Tick)) {#ec::OnNetTick(\&Tick); if (false) {#"
runm ecfx_free_after   $C "/^      run(r);$/{n;s#Free(q, best);#(void)0;#}"
runm ecfx_free_before  $C "/^      run(r);$/{N;s#      run(r);\n      Free(q, best);#      Free(q, best);\n      run(r);#}"
runm ecfx_never_run    $C "s#^      run(r);\$#      (void)r; (void)run;#"
# #2349 D5: EC runs each entry as the drain reaches its slot; this mutant plans the whole queue first, then runs
runm ecfx_plan_then_run $C "s#^    uint64_t ran\[kSlots\];#    uint64_t ran[kSlots]; Run later[kSlots]; int nlater = 0;#;/^      run(r);$/{N;s#      run(r);\n      Free(q, best);#      later[nlater++] = r;#};s#^    return n;#    for (int k = 0; k < nlater; ++k) { run(later[k]); Free(q, later[k].slot); }\n    return n;#"
runm ecfx_empty_slot   $C "s#wall\[i\] == w \&\& at\[i\] \&\& #wall[i] == w \&\& #"
runm ecfx_named_exact  $F "s# \&\& reinterpret_cast<std::uint64_t>(di.dli_saddr) == fn##"
runm ecfx_switch       $F "s#  if (!on) {#  if (!on \&\& false) {#"
runm ecfx_needs_ec     $F "s#  } else if (!ec::Installed()) {#  } else if (false) {#"
runm ecfx_once         $F "s#  } else if (g_installed.exchange(true)) {#  } else if (false) {#"
runm ecfx_legacy       $F "s#  if (legacy::Present(\"neutronfx.on\")) {#  if (false) {#"
# off by its key is counted off on the phase line (combat_install_test)
runm ecfx_off_not_counted $F "s#  return {ok, token, !on};#  return {ok, token, false};#" quest-host-combat combat_install_test
