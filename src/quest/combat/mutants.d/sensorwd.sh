# sensorwd: each rule of src/quest/combat/sensorwd.cpp, sensorwd.h and sensorwd_targets.h removed or bent once; src/quest/tests/sensorwd_test.cpp must fail on every one.
# Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target>]`.
S=src/quest/combat/sensorwd.cpp
H=src/quest/combat/sensorwd.h
T=src/quest/combat/sensorwd_targets.h
CI=src/quest/combat/combat_install.cpp
# the constants of the game and of the rule
runm sd_sphere_id       $S "s/kSphere = 0xac2d3f9f080ba45fULL/kSphere = 0xac2d3f9f080ba45eULL/"
runm sd_triggered_id    $S "s/kTriggered = 0xfd8cf56544b904a3ULL/kTriggered = 0xfd8cf56544b904a2ULL/"
runm sd_finished_id     $S "s/kFinished = 0x7cc621b8bf3248c5ULL/kFinished = 0x7cc621b8bf3248c4ULL/"
runm sd_watchdog_ms     $H "s/kWatchdogMs = 6000;/kWatchdogMs = 5000;/"
runm sd_slots           $H "s/kSlots = 32;/kSlots = 33;/"
runm sd_pings           $H "s/kPings = 8; /kPings = 9; /"
runm sd_pair_window     $S "s/kPairMs = 250; /kPairMs = 2500;/"
runm sd_cs_index_offset $S "s/kCsIndex = 0xd0; /kCsIndex = 0xd8; /"
runm sd_model_name_offset $S "s/kModelName = 0xcc8;/kModelName = 0xcd0;/"
# the pinned slots and words
runm sd_enabled_slot    $T "s/kPins = {0x36495c8ULL/kPins = {0x36495c0ULL/"
runm sd_event_fn        $T "s/0x36fcae8ULL, 0x180735cULL}/0x36fcae8ULL, 0x1807350ULL}/"
runm sd_pin_first_word  $T "s/{0x15b127c, 0xf9406808}/{0x15b127c, 0xf9406809}/"
runm sd_pin_event_word  $T "s/{0x1807388, 0xd65f03c0}/{0x1807388, 0xd65f03c1}/"
runm sd_target_enabled_wrong_expected $T "s/base + pins.enabledFn);/base + pins.enabledFn + 4);/"
runm sd_target_event_wrong_expected $T "s/base + pins.eventFn);/base + pins.eventFn + 4);/"
runm sd_pins_ignored    $S "s/  if (FirstPinMismatch(reinterpret_cast<const unsigned char\*>(image.base)) != nullptr) return refuse(\"pin_mismatch\", \"none\", \"none\", \"none\");/  (void)FirstPinMismatch;/"
# the install: all three or none
runm sd_off_installs    $S "s/  if (!configOn) {/  if (!configOn \&\& pins.enabledSlot == 0) {/"
runm sd_off_not_off     $S "s/    return {false, \"off_config\", true};/    return {false, \"off_config\", false};/"
runm sd_refusal_is_off  $S "s/    return {false, reason};/    return {false, reason, true};/"
runm sd_undo_event_after_enabled $S "s/^    Undo(2);\$/    (void)0;/"
runm sd_undo_enabled_after_disabled $S "s/^    Undo(0);\$/    (void)0;/"
runm sd_undo_after_full_tick $S "s/^      Undo(1);\$/      (void)0;/"
runm sd_no_tick         $S "s/    if (!tick::Add(\&Tick)) {/    if (false) {/"
runm sd_legacy_not_said $S "s/  if (!legacy::Present(\"sensorwd.off\")) return;/  return;/"
# the scope: where a handler acts (the combat level answer, combat_scope.h; its own rules, the arena bit and the staleness bound, are combat_scope's mutants)
runm sd_scope_always    $S "s/^bool InScope() noexcept { return scope::InCombatMap(); }\$/bool InScope() noexcept { return true; }/"
runm sd_scope_stale_believed $S "s/^bool InScope() noexcept { return scope::InCombatMap(); }\$/bool InScope() noexcept { return scope::SeenCombatMap(); }/"
runm sd_scope_lobby     $S "s/^bool InScope() noexcept { return scope::InCombatMap(); }\$/bool InScope() noexcept { return scope::InCombatOrLobby(); }/"
runm sd_scope_arena_wins $S "s/^bool InScope() noexcept { return scope::InCombatMap(); }\$/bool InScope() noexcept { return (scope::detail::g_bits.load() \& scope::kBitCombatMap) != 0 \&\& scope::LookIsFresh(); }/"
runm sd_scope_live_unchecked $S "s/^  if (!scope::Live()) return refuse(\"scope_not_live\", \"none\", \"none\", \"none\");\$//"
runm sd_scope_live_token $S "s/  if (!scope::Live()) return refuse(\"scope_not_live\",/  if (!scope::Live()) return refuse(\"module_not_loaded\",/"
runm sd_scope_live_after_image $S "/^  if (!scope::Live()) return refuse/d; s/^  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return refuse(\"build_id_mismatch\", \"none\", \"none\", \"none\");\$/&\n  if (!scope::Live()) return refuse(\"scope_not_live\", \"none\", \"none\", \"none\");/"
# the tick's reading of the scope: the edge it sees, the table it empties, its line
runm sd_tick_ignores_scope $S "s/^  if (!in) return;\$//"
runm sd_tick_scope_always $S "s/^  const bool in = InScope();\$/  const bool in = true;/"
runm sd_tick_scope_stale_believed $S "s/^  const bool in = InScope();\$/  const bool in = scope::SeenCombatMap();/"
runm sd_leave_keeps_table $S "s/^    if (was) ClearTable();\$//"
runm sd_scope_line_quiet $S "s/^  if (in != was) {\$/  if (false) {/"
runm sd_scope_edge_kept  $S "s/  const bool was = g_wasInScope.exchange(in, std::memory_order_relaxed);/  const bool was = g_wasInScope.load(std::memory_order_relaxed);/"
runm sd_stat_in_scope    $S "s/  s.inScope = g_wasInScope.load();/  s.inScope = false;/"
# the handlers
runm sd_enabled_result  $S "/ReportModel(true, seen);/{n;s/  return result;/  return result \& 0xff;/}"
runm sd_disabled_result $S "/ReportModel(false, seen);/{n;s/  return result;/  return result \& 0xff;/}"
runm sd_event_result    $S "/ReportEvent(seen, a, b);/{n;s/  return result;/  return result \& 0xff;/}"
runm sd_enabled_is_disable $S "s/  const Seen seen = TrackModel(true, reinterpret_cast<std::uintptr_t>(component), handle);/  const Seen seen = TrackModel(false, reinterpret_cast<std::uintptr_t>(component), handle);/"
runm sd_disabled_is_enable $S "s/  const Seen seen = TrackModel(false, reinterpret_cast<std::uintptr_t>(component), handle);/  const Seen seen = TrackModel(true, reinterpret_cast<std::uintptr_t>(component), handle);/"
runm sd_event_args      $S "s/  const std::uint64_t result = original(gamespace, symbol, a, b, flags);/  const std::uint64_t result = original(gamespace, symbol, b, a, flags);/"
runm sd_event_flags     $S "s/  const std::uint64_t result = original(gamespace, symbol, a, b, flags);/  const std::uint64_t result = original(gamespace, symbol, a, b, 0);/"
runm sd_ping_after_game $S "s/^  const Seen seen = TrackEvent(symbol, a, b);/  Seen seen;/;s/^  const std::uint64_t result = original(gamespace, symbol, a, b, flags);/&  seen = TrackEvent(symbol, a, b);/"
# the sphere test and the table
runm sd_is_sphere_name  $S "s/  return name == kSphere;/  return name != kSphere;/"
runm sd_not_sphere_counted $S "s/  if (!IsSphere(cs, h)) return seen;/  (void)0;/"
runm sd_pair_needs_empty $S "s/      if (s->actA == 0 \&\& s->actB == 0 \&\& now - g_lastMs <= kPairMs) {/      if (now - g_lastMs <= kPairMs) {/"
runm sd_pair_window_edge $S "s/now - g_lastMs <= kPairMs/now - g_lastMs < 0/"
runm sd_pair_never      $S "s/now - g_lastMs <= kPairMs/false/"
runm sd_enable_forced_kept $S "s/      s->forced = false;\n      s->enMs/&/;0,/^      s->forced = false;\$/s//      (void)0;/"
runm sd_disable_clears  $S "s/    s->enabled = false;\n    s->forced = false;/&/;0,/^    s->enabled = false;\$/s//    (void)0;/"
runm sd_zero_ping_queued $S "s/    if (a != 0 || b != 0) {/    if (true) {/"
runm sd_ping_queue_wraps $S "s/      if (g_pingN < kPings) {/      if (true) {/"
runm sd_ping_lost_uncounted $S "s/    if (seen.lost) g_pingsLost.fetch_add(1, std::memory_order_relaxed);//"
runm sd_finished_uncounted $S "s/    g_finished.fetch_add(1, std::memory_order_relaxed);//"
runm sd_table_full_uncounted $S "s/  if (seen.full) g_tableFull.fetch_add(1, std::memory_order_relaxed);//"
# the tick
runm sd_wd_edge         $S "s/    } else if (s.enabled \&\& now - s.enMs >= kWatchdogMs) {/    } else if (s.enabled \&\& now - s.enMs > kWatchdogMs) {/"
runm sd_wd_not_enabled  $S "s/    } else if (s.enabled \&\& now - s.enMs >= kWatchdogMs) {/    } else if (now - s.enMs >= kWatchdogMs) {/"
runm sd_forced_unset    $S "s/      s.forced = true;\n      s.enabled = false;/&/;0,/^      s.forced = true;\$/s//      s.forced = false;/"
runm sd_back_match_or   $S "s/        if (g_ping\[i\].a == s.actA \&\& g_ping\[i\].b == s.actB) back = true;/        if (g_ping[i].a == s.actA || g_ping[i].b == s.actB) back = true;/"
runm sd_back_only_forced $S "s/    if (s.forced) {  \/\/ a sphere with no actor pair/    if (true) {  \/\/ a sphere with no actor pair/"
runm sd_back_restarts_clock $S "s/      s.enMs = now;\n      act/&/;0,/^      s.enMs = now;\$/s//      (void)now;/"
runm sd_pings_kept      $S "s/^  g_pingN = 0;\$/  (void)g_pingN;/"
runm sd_tick_calls_wrong_fn $S "s/    const tgt::EnabledThunk::Fn fn = x.on ? tgt::EnabledThunk::Original() : tgt::DisabledThunk::Original();/    const tgt::EnabledThunk::Fn fn = x.on ? tgt::DisabledThunk::Original() : tgt::EnabledThunk::Original();/"
runm sd_tick_call_args  $S "s/    (void)fn(reinterpret_cast<void\*>(x.cs), x.h);/    (void)fn(reinterpret_cast<void*>(x.cs), 0);/"
runm sd_stale_not_dropped $S "s/    if (!IsSphere(x.cs, x.h)) {  \/\/ the component is gone or recycled/    if (!IsSphere(x.cs, x.h) \&\& x.h == 0) {  \/\/ the component is gone or recycled/"
runm sd_sweep_never     $S "s/    if (!IsSphere(keys\[i\].cs, keys\[i\].h)) dead\[nd++\] = keys\[i\];/    (void)dead;/"
runm sd_sweep_period    $S "s/    g_nextSweepMs.store(now + kSweepMs, std::memory_order_relaxed);/    g_nextSweepMs.store(now + kSweepMs * 100, std::memory_order_relaxed);/"
runm sd_drop_keeps_slot $S "s/  if (Slot\* s = Find(cs, h, false)) \*s = Slot{0, 0, 0, 0, 0, false, false};/  (void)Find(cs, h, false);/"
runm sd_stat_wdoff      $S "s/    const std::uint32_t c = (x.on ? g_wdBack : g_wdOff).fetch_add(1, std::memory_order_relaxed) + 1;/    const std::uint32_t c = (x.on ? g_wdOff : g_wdBack).fetch_add(1, std::memory_order_relaxed) + 1;/"
runm sd_off_line_age    $S "s/{\"age_ms\", x.age}/{\"age_ms\", 0}/"
runm sd_first_every     $S "s/  if (g_firstSaid.fetch_or(bit, std::memory_order_acq_rel) \& bit) return;/  if (g_firstSaid.fetch_or(bit, std::memory_order_acq_rel) \& bit \& 0) return;/"
runm sd_lock_across_game $S "s/  const Seen seen = TrackModel(true, reinterpret_cast<std::uintptr_t>(component), handle);/  Lock(); const Seen seen = TrackModel(true, reinterpret_cast<std::uintptr_t>(component), handle); Unlock();/"
# the switch's key and row (shared files)
runm sd_ci_row_ignores_key $CI "s/sensorwd::Install(g_config.sensorwd.on)/sensorwd::Install(true)/" quest-host-combat combat_install_test
runm sd_qc_default_off  src/quest/sentinel/quest_config.cpp "s/  r.config.combat.sensorwd = ReadCombatSwitch(cfg, kCombatSensorwdKey, r);/  r.config.combat.sensorwd = ReadCombatSwitch(cfg, kCombatSensorwdKey, r, false);/" quest-host-shared quest_config_test
runm sd_qc_not_read     src/quest/sentinel/quest_config.cpp "s/  r.config.combat.sensorwd = ReadCombatSwitch(cfg, kCombatSensorwdKey, r);/  r.config.combat.sensorwd = CombatSwitch{};/" quest-host-shared quest_config_test
runm sd_ci_row_key       $CI "s/\"combat.sensorwd\", \&nevr_quest::CombatConfig::sensorwd}/\"combat.sensorwdd\", \&nevr_quest::CombatConfig::sensorwd}/" quest-host-combat combat_install_test
# arena pays nothing, by count (#2277 review): the sphere check (five reads) before the scope test, in each handler
runm sd_read_before_scope_enabled $S "/^std::uint64_t OnEnabled/{n;s/^/  (void)IsSphere(reinterpret_cast<std::uintptr_t>(component), handle);\n/}"
runm sd_read_before_scope_disabled $S "/^std::uint64_t OnDisabled/{n;s/^/  (void)IsSphere(reinterpret_cast<std::uintptr_t>(component), handle);\n/}"
runm sd_scan_in_event_handler $S "s/^#include \"quest\/combat\/combat_scope.h\"\$/&\n#include \"quest\/combat\/combat_level.h\"/; /^std::uint64_t OnEvent/{n;s/^/  (void)level::Classify(0, false, nullptr);\n/}"
# The scope row's list of readers (combat_install.cpp, a shared file: run while holding the shared lock): the watchdog keeps the row on alone.
runm sdrow_scope_reader $CI 's/\(|| \|^ *\)g_config\.sensorwd\.on/\1false/' quest-host-combat combat_install_test
