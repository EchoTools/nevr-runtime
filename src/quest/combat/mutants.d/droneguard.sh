# droneguard: each rule of src/quest/combat/droneguard.cpp and droneguard_targets.h removed or bent once; src/quest/tests/droneguard_test.cpp must fail
# on every one (the shared files it changed are each run against the test that covers them). Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
D=src/quest/combat/droneguard.cpp
T=src/quest/combat/droneguard_targets.h
H=src/quest/combat/droneguard.h
# stage 0: a null target is not followed, anything else is
runm dg_no_null_check     $D "s/  if (s.target == 0) {/  if (false) {/"
runm dg_always_skips      $D "s/  if (s.target == 0) {/  if (true) {/"
runm dg_handle_off        $T "s/kOffHandle = 0x148,/kOffHandle = 0x140,/"
runm dg_flags_off         $T "s/kOffHandleFlags = 0x16c,/kOffHandleFlags = 0x168,/"
runm dg_sys_off           $T "s/kOffHandleSys = 0x188,/kOffHandleSys = 0x180,/"
runm dg_arg_handle        $D "s/^  original(self, handle);\$/  original(self, 0);/"
runm dg_arg_cs            $D "s/^  original(self, handle);\$/  original(nullptr, handle);/"
runm dg_no_original       $D "s/^  original(self, handle);\$/  (void)original;/"
# the state line
runm dg_state_every       $D "s/  if (g_state.exchange(key, std::memory_order_relaxed) == key) return;/  g_state.store(key);/"
runm dg_state_cap         $H "s/kStateLines = 16;/kStateLines = 17;/"
runm dg_key_idx           $D "s/const std::uint64_t key = s.idx |/const std::uint64_t key = 0 |/"
runm dg_key_flags         $D "s/(static_cast<std::uint64_t>(s.flags) << 32)/0/"
runm dg_key_target        $D "s/(static_cast<std::uint64_t>(s.target != 0) << 40)/0/"
runm dg_key_sys           $D "s/(static_cast<std::uint64_t>(s.sys != 0) << 41)/0/"
runm dg_state_unresolved  $D "s/(s.idx \& 0xffff) == 0xffff ? \"yes\" : \"no\"/s.idx == 0xffff ? \"yes\" : \"no\"/"
runm dg_state_level       $D "s/sentinel::LogLevel::kInfo, \"combat_droneguard_state\"/sentinel::LogLevel::kDebug, \"combat_droneguard_state\"/"
# the skip lines and totals
runm dg_total_every       $H "s/kTotalEvery = 600;/kTotalEvery = 599;/"
runm dg_total_lines       $H "s/kTotalLines = 8;/kTotalLines = 9;/"
runm dg_skip_uncounted    $D "s/const unsigned n = g_skips.fetch_add(1, std::memory_order_relaxed) + 1;/const unsigned n = g_skips.fetch_add(0, std::memory_order_relaxed) + 1;/"
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
runm dg_idx_resolved_n    $D "s/\"combat_droneguard_idx_resolved\", {{\"idx_skips\", g_idxSkips.load(std::memory_order_relaxed)}}/\"combat_droneguard_idx_resolved\", {{\"idx_skips\", 0}}/"
runm dg_idx_uncounted     $D "s/const unsigned n = g_idxSkips.fetch_add(1, std::memory_order_relaxed) + 1;/const unsigned n = g_idxSkips.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm dg_idx_why_lost      $D "s/{{\"why\", why}, {\"cs\"/{{\"why\", why ? \"idx ffff\" : \"idx ffff\"}, {\"cs\"/"
# stage 1 preconditions: its words, both functions, the exact addresses
runm dg_s1_pins_skipped   $D "s/  if (pinned::FirstFailure(base, \&kIdxWordPins, 1, \&word)) {/  if (pinned::FirstFailure(base, \&kIdxWordPins, 0, \&word)) {/"
runm dg_s1_last_pin       $T "s/kIdxWordPins = {kIdxWords, kIdxWordCount, nullptr, 0};/kIdxWordPins = {kIdxWords, kIdxWordCount - 1, nullptr, 0};/"
runm dg_s1_unreadable_ok  $D "s/{\"found\", word.readable ? sentinel::HexString(haveText, word.have) : \"unreadable\"}/{\"found\", sentinel::HexString(haveText, word.have)}/"
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
runm dg_pins_skipped        $D "s/  if (pinned::FirstFailure(image.base, \&kWordPins, 1, \&word)) return Refused(\"pinned_word_mismatch\", \&word);/  if (false) return Refused(\"pinned_word_mismatch\", \&word);/"
runm dg_last_pin_skipped    $T "s/kWordPins = {kWords, kWordCount, nullptr, 0};/kWordPins = {kWords, kWordCount - 1, nullptr, 0};/"
runm dg_pin_unreadable_ok $D "s/word->readable ? sentinel::HexString(haveText, word->have) : \"unreadable\"/sentinel::HexString(haveText, word->have)/"
runm dg_build_id_skipped    $D "s/  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\");//"
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
runm dg_row_end_null      $CI "s/droneguard, \&droneguard::NoteEnd}/droneguard, nullptr}/" quest-host-combat combat_install_test
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

# the second stage is a row of its own: its key and its member (combat_install.cpp)
runm dg_stage1_row_member  $CI "s/\"combat.droneguard_stage1\", \&nevr_quest::CombatConfig::droneguardStage1}/\"combat.droneguard_stage1\", \&nevr_quest::CombatConfig::droneguard}/" quest-host-combat combat_install_test
runm dg_stage1_row_key     $CI "s/\"combat.droneguard_stage1\", \&nevr_quest/\"combat.droneguard_stage\", \&nevr_quest/" quest-host-combat combat_install_test
runm dg_stage1_row_step    $CI "s/Module{\"droneguard_stage1\", nullptr, nullptr,/Module{\"droneguard_stage1\", nullptr, \&DroneguardNormal,/" quest-host-combat combat_install_test
runm dg_row_member         $CI "s/\"combat.droneguard\", \&nevr_quest::CombatConfig::droneguard,/\"combat.droneguard\", \&nevr_quest::CombatConfig::voipdrop,/" quest-host-combat combat_install_test
runm dg_row_key            $CI "s/\"combat.droneguard\", \&nevr_quest/\"combat.droneguards\", \&nevr_quest/" quest-host-combat combat_install_test
# droneguard asks nothing of the scope: it is not a reader of the scope row
runm dg_reader_listed      $CI "s/ || g_config.custctx.on;/ || g_config.custctx.on || g_config.droneguard.on;/" quest-host-combat combat_install_test
# (kOffTarget and kOffHandleIdx are tied to the pinned words by static_assert in droneguard_targets.h: a changed one fails the build)
# the run of skips belongs to the component, and each kind of run line has a cap that says so (review F1)
runm dg_run_shared          $D "s/  Began Begin(std::uintptr_t c) noexcept {/  Began Begin(std::uintptr_t c) noexcept {\n    if (c != 0) return Began::kNew;/"
runm dg_run_end_always      $D "s/  Ended End(std::uintptr_t c) noexcept {/  Ended End(std::uintptr_t c) noexcept {\n    if (c != 0) return Ended::kEnded;/"
runm dg_run_end_never       $D "s/  Ended End(std::uintptr_t c) noexcept {/  Ended End(std::uintptr_t c) noexcept {\n    if (c != 0) return Ended::kNone;/"
runm dg_run_slots_one       $H "s/kRunSlots = 16; /kRunSlots = 1;  /"
runm dg_idx_run_shared      $D "s/  const Began began = g_idxRuns.Begin(cs);/  const Began began = g_idxRuns.Begin(cs) == Began::kFull ? Began::kFull : Began::kNew;/"
runm dg_idx_run_end_always  $D "s/    if (g_idxRuns.End(cs) == Ended::kEnded \&\&/    if (true \&\&/"
runm dg_skip_cap_ignored    $D "s/    if (LineBudget(\&g_skipLines, kRunLines,/    if (true || LineBudget(\&g_skipLines, kRunLines,/"
runm dg_idx_skip_cap_ignored $D "s/    if (LineBudget(\&g_idxSkipLines, kRunLines,/    if (true || LineBudget(\&g_idxSkipLines, kRunLines,/"
runm dg_state_cap_ignored   $D "s/  if (!LineBudget(\&g_stateLines, kStateLines, \"droneguard\", \"state\", \"[^\"]*\")) return;/  if (g_stateLines.fetch_add(1, std::memory_order_relaxed) >= kStateLines) return;/"
runm dg_totals_budget_ignored $D "s/             LineBudget(\&g_totals, kTotalLines, \"droneguard\", \"skips\", \"[^\"]*\")) {/             g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {/"
runm dg_run_lines_const     $H "s/kRunLines = 8; /kRunLines = 9; /"

runm dg_set_cap_ignored     $D "s/      LineBudget(\&g_targetSetLines, kRunLines,/      LineBudget(\&g_targetSetLines, 1000000,/"
runm dg_idx_set_cap_ignored $D "s/        LineBudget(\&g_idxResolvedLines, kRunLines,/        LineBudget(\&g_idxResolvedLines, 1000000,/"
runm dg_run_begin_never    $D "s/  Began Begin(std::uintptr_t c) noexcept {/  Began Begin(std::uintptr_t c) noexcept {\n    if (c != 0) return Began::kInRun;/"
runm dg_idx_totals_budget_ignored $D "s/             LineBudget(\&g_idxTotals, kTotalLines, \"droneguard\", \"idx_skips\", \"[^\"]*\")) {/             g_idxTotals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {/"
# the run table is asked only while a run is open (review: arena cost), a run's slot is released when it ends or goes stale, and a full table says so
runm dg_early_out_removed   $D "s/    if (open.load(std::memory_order_relaxed) == 0) return Ended::kNone;/    (void)0;/"
runm dg_open_never_counted  $D "s/    open.fetch_add(1, std::memory_order_relaxed);/    (void)0;/"
runm dg_open_never_released $D "s/      open.fetch_sub(1, std::memory_order_relaxed);/      (void)0;/"
runm dg_stale_never_at_end  $D "s/      const bool stale = NowMs() - since\[i\].load(std::memory_order_relaxed) > kStaleMs;/      const bool stale = false;/"
runm dg_stale_always_at_end $D "s/      const bool stale = NowMs() - since\[i\].load(std::memory_order_relaxed) > kStaleMs;/      const bool stale = true;/"
runm dg_reclaim_off         $D "s/        if (now - since\[i\].load(std::memory_order_relaxed) > kStaleMs) {/        if (false) {/"
runm dg_reclaim_always      $D "s/        if (now - since\[i\].load(std::memory_order_relaxed) > kStaleMs) {/        if (true) {/"
runm dg_stale_end_uncounted $D "s/      if (stale) g_runsStale.fetch_add(1, std::memory_order_relaxed);/      (void)0;/"
runm dg_stale_reclaim_uncounted $D "s/^          g_runsStale.fetch_add(1, std::memory_order_relaxed);/          (void)0;/"
runm dg_stale_window_ms     $D "s/static_cast<std::int64_t>(kRunStaleSeconds) \* 1000;/static_cast<std::int64_t>(kRunStaleSeconds);/"
runm dg_since_not_set       $D "s/^    since\[freeAt\].store(now, std::memory_order_relaxed);/    (void)0;/"
runm dg_full_silent         $D "s/  if (g_fullSaid.exchange(true, std::memory_order_relaxed)) return;/  if (true) return;/"
runm dg_full_every_time     $D "s/  if (g_fullSaid.exchange(true, std::memory_order_relaxed)) return;/  if (false) return;/"
runm dg_full_uncounted      $D "s/  g_runTableFull.fetch_add(1, std::memory_order_relaxed);/  (void)0;/"
runm dg_full_idx_silent     $D "s/  if (began == Began::kFull) NoteTableFull(\"idx_skip\");/  (void)0;/"
runm dg_stats_runs_uncounted $D "s/^    g_runs.fetch_add(1, std::memory_order_relaxed);/    (void)0;/"
runm dg_stats_target_set_uncounted $D "s/  if (ended == Ended::kEnded) g_targetSets.fetch_add(1, std::memory_order_relaxed);/  (void)0;/"
runm dg_stats_line_no_runs  $D "s/^                       {\"runs\", g_runs.load(std::memory_order_relaxed)},//"
runm dg_stats_line_no_target_set $D "s/^                       {\"target_set\", g_targetSets.load(std::memory_order_relaxed)},//"
