# droneguard: each rule of src/quest/combat/droneguard.cpp and droneguard_targets.h removed or bent once; src/quest/tests/droneguard_test.cpp must fail
# on every one (the shared files it changed are each run against the test that covers them). Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
D=src/quest/combat/droneguard.cpp
T=src/quest/combat/droneguard_targets.h
H=src/quest/combat/droneguard.h
# stage 0: a null target is not followed, anything else is
runm dg_no_null_check     $D "s/  if (s.target == 0) {/  if (false) {/"
runm dg_always_skips      $D "s/  if (s.target == 0) {/  if (true) {/"
runm dg_target_off        $T "s/kOffTarget = 0x140,/kOffTarget = 0x138,/"
runm dg_handle_off        $T "s/kOffHandle = 0x148,/kOffHandle = 0x140,/"
runm dg_flags_off         $T "s/kOffHandleFlags = 0x16c,/kOffHandleFlags = 0x168,/"
runm dg_sys_off           $T "s/kOffHandleSys = 0x188,/kOffHandleSys = 0x180,/"
runm dg_idx_off           $T "s/kOffHandleIdx = 0x190;/kOffHandleIdx = 0x18c;/"
runm dg_arg_handle        $D "s/^  original(self, handle);\$/  original(self, 0);/"
runm dg_arg_cs            $D "s/^  original(self, handle);\$/  original(nullptr, handle);/"
runm dg_no_original       $D "s/^  original(self, handle);\$/  (void)original;/"
runm dg_unread_skips      $D "/  if (!ReadSnap(cs, \&s)) {  \/\/ doubt/{n;s/    original(self, handle);/    (void)original;/}"
runm dg_unread_ignored    $D "s/  if (!ReadSnap(cs, \&s)) {  \/\/ doubt: Follow decides.*/  (void)ReadSnap(cs, \&s); if (false) {/"
# the state line
runm dg_state_every       $D "s/  if (g_state.exchange(key, std::memory_order_relaxed) == key) return;/  g_state.store(key);/"
runm dg_state_cap         $H "s/kStateLines = 16;/kStateLines = 17;/"
runm dg_state_no_cap      $D "s/  if (g_stateLines.fetch_add(1, std::memory_order_relaxed) >= kStateLines) return;/  (void)g_stateLines;/"
runm dg_key_idx           $D "s/const std::uint64_t key = s.idx |/const std::uint64_t key = 0 |/"
runm dg_key_flags         $D "s/(static_cast<std::uint64_t>(s.flags) << 32)/0/"
runm dg_key_target        $D "s/(static_cast<std::uint64_t>(s.target != 0) << 40)/0/"
runm dg_key_sys           $D "s/(static_cast<std::uint64_t>(s.sys != 0) << 41)/0/"
runm dg_state_unresolved  $D "s/(s.idx \& 0xffff) == 0xffff ? \"yes\" : \"no\"/s.idx == 0xffff ? \"yes\" : \"no\"/"
runm dg_state_level       $D "s/sentinel::LogLevel::kInfo, \"combat_droneguard_state\"/sentinel::LogLevel::kDebug, \"combat_droneguard_state\"/"
# the skip lines and totals
runm dg_first_every       $D "s/  if (!g_skipping.exchange(true, std::memory_order_relaxed)) {/  if (true) {/"
runm dg_first_never       $D "s/  if (!g_skipping.exchange(true, std::memory_order_relaxed)) {/  if (false) {/"
runm dg_total_every       $H "s/kTotalEvery = 600;/kTotalEvery = 599;/"
runm dg_total_lines       $H "s/kTotalLines = 8;/kTotalLines = 9;/"
runm dg_total_no_cap      $D "s/ \&\& g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {/) {/"
runm dg_skip_uncounted    $D "s/const unsigned n = g_skips.fetch_add(1, std::memory_order_relaxed) + 1;/const unsigned n = g_skips.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm dg_no_target_set     $D "s/  if (g_skipping.exchange(false, std::memory_order_relaxed)) {/  if (false) {/"
runm dg_target_set_skips  $D "s/\"combat_droneguard_target_set\", {{\"skips\", g_skips.load(std::memory_order_relaxed)}}/\"combat_droneguard_target_set\", {{\"skips\", 0}}/"
# stage 1
runm dg_idx_ignored       $D "s/  if (g_idxOn.load(std::memory_order_relaxed)) {/  if (false) {/"
runm dg_idx_always        $D "s/  if (g_idxOn.load(std::memory_order_relaxed)) {/  if (true) {/"
runm dg_idx_no_refresh    $D "s/  g_f.refresh(reinterpret_cast<void\*>(cs + kOffHandle));/  (void)cs;/"
runm dg_idx_refresh_arg   $D "s/  g_f.refresh(reinterpret_cast<void\*>(cs + kOffHandle));/  g_f.refresh(reinterpret_cast<void*>(cs));/"
runm dg_idx_unread        $D "s/  if (!ReadSnap(cs, \&s)) return nullptr;  \/\/ doubt: Follow runs/  (void)ReadSnap(cs, \&s);/"
runm dg_idx_unread_skips  $D "s/  if (!ReadSnap(cs, \&s)) return nullptr;  \/\/ doubt: Follow runs/  if (!ReadSnap(cs, \&s)) return \"idx ffff\";/"
runm dg_idx_ffff_all      $D "s/  if ((s.idx \& 0xffff) == 0xffff) return \"idx ffff\";/  if (s.idx == 0xffff) return \"idx ffff\";/"
runm dg_idx_ffff_none     $D "s/  if ((s.idx \& 0xffff) == 0xffff) return \"idx ffff\";/  (void)0;/"
runm dg_idx_sys0          $D "s/  if (s.sys == 0) return \"handle-sys 0\";/  (void)0;/"
runm dg_idx_valid         $D "s/  if (!g_f.isValid(reinterpret_cast<const void\*>(s.target), s.idx)) return \"refused but cached\";/  (void)0;/"
runm dg_valid_target      $D "s/isValid(reinterpret_cast<const void\*>(s.target), s.idx)/isValid(reinterpret_cast<const void*>(s.sys), s.idx)/"
runm dg_valid_idx         $D "s/isValid(reinterpret_cast<const void\*>(s.target), s.idx)/isValid(reinterpret_cast<const void*>(s.target), s.idx \& 0xffff)/"
runm dg_idx_first_every   $D "s/  if (!g_idxSkipping.exchange(true, std::memory_order_relaxed)) {/  if (true) {/"
runm dg_idx_no_resolved   $D "s/    if (g_idxSkipping.exchange(false, std::memory_order_relaxed)) {/    if (false) {/"
runm dg_idx_resolved_n    $D "s/\"combat_droneguard_idx_resolved\", {{\"idx_skips\", g_idxSkips.load(std::memory_order_relaxed)}}/\"combat_droneguard_idx_resolved\", {{\"idx_skips\", 0}}/"
runm dg_idx_total_every   $D "s/} else if (n % kTotalEvery == 0 \&\& g_idxTotals/} else if (n % 601 == 0 \&\& g_idxTotals/"
runm dg_idx_total_no_cap  $D "s/ \&\& g_idxTotals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {/) {/"
runm dg_idx_uncounted     $D "s/const unsigned n = g_idxSkips.fetch_add(1, std::memory_order_relaxed) + 1;/const unsigned n = g_idxSkips.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm dg_idx_why_lost      $D "s/{{\"why\", why}, {\"cs\"/{{\"why\", why ? \"idx ffff\" : \"idx ffff\"}, {\"cs\"/"
# stage 1 preconditions: its words, both functions, the exact addresses
runm dg_s1_pins_skipped   $D "s/  if (bad != kIdxWordCount) {/  if (false) {/"
runm dg_s1_last_pin       $D "s/FirstWordMismatch(base, kIdxWords, kIdxWordCount, \&have, \&readable)/FirstWordMismatch(base, kIdxWords, kIdxWordCount - 1, \&have, \&readable)/"
runm dg_s1_unreadable_ok  $D "s/{\"found\", readable ? sentinel::HexString(haveText, have) : \"unreadable\"}/{\"found\", sentinel::HexString(haveText, have)}/"
runm dg_s1_fns_check      $D "s/  if (fns == nullptr || fns->refresh == nullptr || fns->isValid == nullptr) {/  if (false) {/"
runm dg_s1_fns_half       $D "s/ || fns->refresh == nullptr || fns->isValid == nullptr) {/) {/"
runm dg_s1_quiet          $D "s/sentinel::LogLevel::kError, \"combat_droneguard_stage1\",/sentinel::LogLevel::kInfo, \"combat_droneguard_stage1\",/"
runm dg_s1_on_not_set     $D "s/  g_idxOn.store(true, std::memory_order_relaxed);  \/\/ before the swap.*/  (void)g_f;/"
runm dg_s1_without_on     $D "s/  const char\* const stageOne = stage1 ? PrepareStageOne(image.base, fns) : \"off\";/  const char* const stageOne = PrepareStageOne(image.base, fns);/"
runm dg_s1_status_plain   $D "s/  return {true, stage1 \&\& std::strcmp(stageOne, \"on\") != 0 ? \"installed_null_half\" : \"installed\"};/  return {true, \"installed\"};/"
runm dg_exports_refresh   $D "s/reinterpret_cast<std::uintptr_t>(refresh) != base + kRefreshCacheVaddr || /false || /"
runm dg_exports_valid     $D "s/ || reinterpret_cast<std::uintptr_t>(valid) != base + kIsValidVaddr) return false;/) return false;/"
runm dg_refresh_const     $T "s/kRefreshCacheVaddr = 0x1210848ULL/kRefreshCacheVaddr = 0x1210840ULL/"
runm dg_valid_const       $T "s/kIsValidVaddr = 0x1cdb3e0ULL/kIsValidVaddr = 0x1cdb3e8ULL/"
runm dg_resolve_no_lib    $D "s/  if (h == nullptr) return false;/  (void)h;/"
# the install: the slot must hold Follow, the stage-0 words and the build ID gate it, refusals are loud
runm dg_slot_const        $T "s/kFollowSlotVaddr = 0x36f1218ULL/kFollowSlotVaddr = 0x36f1210ULL/"
runm dg_fn_const          $T "s/kFollowVaddr = 0x2149764ULL/kFollowVaddr = 0x2149760ULL/"
runm dg_no_expected       $D "s/  target.expectedOriginal = reinterpret_cast<const void\*>(image.base + pins.fn);/  (void)pins.fn;/"
runm dg_expected_wrong    $D "s/image.base + pins.fn);/image.base + pins.fn + 4);/"
runm dg_pins_skipped      $D "s/  if (bad != kWordCount) {/  if (false) {/"
runm dg_last_pin_skipped  $D "s/FirstWordMismatch(image.base, kWords, kWordCount, \&have, \&readable)/FirstWordMismatch(image.base, kWords, kWordCount - 1, \&have, \&readable)/"
runm dg_pin_unreadable_ok $D "s/readable ? sentinel::HexString(haveText, have) : \"unreadable\", sentinel::HexString(wantText, kWords/sentinel::HexString(haveText, have), sentinel::HexString(wantText, kWords/"
runm dg_build_id_skipped  $D "s/  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\");//"
runm dg_no_build_id_ok    $D "s/  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\");//"
runm dg_refusal_quiet     $D "s/sentinel::LogFields(sentinel::LogLevel::kError, \"combat_droneguard_install\",/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_droneguard_install\",/"
runm dg_refused_stage1_on $D "s/  if (!installed \&\& !g_hook.installed()) g_idxOn.store(false, std::memory_order_relaxed);//"
runm dg_install_level     $D "s/sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_droneguard_install\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_droneguard_install\"/"
# the switch
runm dg_off_ignored       $D "s/  if (!on) {/  if (!on \&\& false) {/"
runm dg_off_not_off       $D "s/    return {false, \"off_config\", true};/    return {false, \"off_config\", false};/"
# the stats
runm dg_stats_never       $D "s/  if ((calls \& (kStatsEvery - 1)) == 0) LogStats(false);/  (void)calls;/"
runm dg_stats_every       $D "s/  if ((calls \& (kStatsEvery - 1)) == 0) LogStats(false);/  if (calls != ~0ULL) LogStats(false);/"
runm dg_stats_every_wrong $H "s/kStatsEvery = 16384;/kStatsEvery = 8192;/"
runm dg_stats_final_never $D "s/  if (g_hook.installed()) LogStats(true);/  (void)g_hook;/"
runm dg_stats_final_always $D "s/  if (g_hook.installed()) LogStats(true);/  LogStats(true);/"
runm dg_stats_final_debug $D "s/final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, \"combat_droneguard_stats\"/sentinel::LogLevel::kDebug, \"combat_droneguard_stats\"/"
runm dg_stats_idx_skips   $D "s/{\"idx_skips\", g_idxSkips.load(std::memory_order_relaxed)},/{\"idx_skips\", 0},/"
# the table row (combat_install.cpp), the key and the schema
CI=src/quest/combat/combat_install.cpp
runm dg_row_ignores_switch $CI "s/droneguard::Install(g_config.droneguard.on, g_config.droneguardStage1.on)/droneguard::Install(true, g_config.droneguardStage1.on)/" quest-host-combat combat_install_test
runm dg_row_end_null      $CI "s/\&DroneguardNormal, \&droneguard::NoteEnd}/\&DroneguardNormal, nullptr}/" quest-host-combat combat_install_test
QC=src/quest/sentinel/quest_config.cpp
runm dg_qc_default_off    $QC "s/  r.config.combat.droneguard = ReadCombatSwitch(cfg, kCombatDroneguardKey, r);/  r.config.combat.droneguard = ReadCombatSwitch(cfg, kCombatDroneguardKey, r, false);/" quest-host-shared quest_config_test
runm dg_qc_stage1_on      $QC "s/ReadCombatSwitch(cfg, kCombatDroneguardStage1Key, r, \/\*fallback=\*\/false)/ReadCombatSwitch(cfg, kCombatDroneguardStage1Key, r)/" quest-host-shared quest_config_test
runm dg_qc_key_typo       $QC "s/kCombatDroneguardKey = \"combat.droneguard\";/kCombatDroneguardKey = \"combat.droneguards\";/" quest-host-shared quest_config_test
runm dg_qc_stage1_typo    $QC "s/kCombatDroneguardStage1Key = \"combat.droneguard_stage1\";/kCombatDroneguardStage1Key = \"combat.droneguard_stage\";/" quest-host-shared quest_config_test
runm dg_qc_not_assigned   $QC "s/  r.config.combat.droneguard = ReadCombatSwitch(cfg, kCombatDroneguardKey, r);/  (void)ReadCombatSwitch(cfg, kCombatDroneguardKey, r);/" quest-host-shared quest_config_test
runm dg_nc_known_key      src/core/nevr_config.cpp "s/\"droneguard\", \"droneguard_stage1\", /\"droneguard_stage1\", /" quest-host-shared quest_config_test
# the fast path is plain loads; a null component goes to the game's function untouched
runm dg_plain_null_unchecked $D "s/  if (cs == 0) return false;//"
runm dg_plain_flags_wrong    $D "s/\*reinterpret_cast<const volatile std::uint8_t\*>(cs + kOffHandleFlags)/*reinterpret_cast<const volatile std::uint8_t*>(cs + kOffHandleFlags + 1)/"
