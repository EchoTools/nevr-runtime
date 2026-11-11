# texture_quest2: each rule of the Quest 2 step-down removed or bent once (#1737, docs/quest/combat/texture_quest2.md): the pure rules
# (src/quest/sentinel/texture_quality_rules.h, against texture_quality_rules_test in the hooks group), the owner's use of them
# (src/quest/sentinel/texture_menu_hook.cpp and texture_quality_hook.cpp, against their hooks tests) and combat's look
# (src/quest/combat/texture_quest2.cpp, against combat_texture_quest2_test). Every file named here is the unit's own; no shared file is mutated.
# Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target>]`.
R=src/quest/sentinel/texture_quality_rules.h
RT="quest-host-hooks texture_quality_rules_test"
# the level sent
runm tq2_no_registered   $R "s/  if (!in.registered) return {in.requested, RuleReason::kOff};//" $RT
runm tq2_no_look_off     $R "s/  if (in.lookOffForRun) return {in.requested, RuleReason::kLookOff};//" $RT
runm tq2_no_quest2       $R "s/  if (!in.quest2) return {in.requested, RuleReason::kNotQuest2};//" $RT
runm tq2_no_player       $R "s/  if (in.playerChose) return {in.requested, RuleReason::kPlayerChoice};//" $RT
runm tq2_gate_inverted   $R "s/  if (in.gate.on) return {kStepDownLevel, RuleReason::kQuest2Combat};/  if (!in.gate.on) return {kStepDownLevel, RuleReason::kQuest2Combat};/" $RT
runm tq2_low_not_vlow    $R "s/kStepDownLevel = Level::kVeryLow;/kStepDownLevel = Level::kLow;/" $RT
# the gate
runm tq2_settle_zero     $R "s/kSettleMs = 10000;/kSettleMs = 0;/" $RT
runm tq2_gap_changed     $R "s/kGapMs = 90000;/kGapMs = 9000;/" $RT
runm tq2_fresh_changed   $R "s/kLookFreshMs = 1500;/kLookFreshMs = 15000;/" $RT
runm tq2_load_not_reset  $R "s/    gate.quietSinceMs = 0;\$/    (void)0;/" $RT
runm tq2_stale_quiet     $R "s/    gate.quietSinceMs = 0;  \/\/ nothing is known about loads now/    (void)0;/" $RT
runm tq2_no_arena_off    $R "s/  } else if (place == Place::kArena \&\& settled) {/  } else if (false) {/" $RT
runm tq2_no_gap_off      $R "s/  } else if (place == Place::kOther \&\& nowMs - gate.placeSinceMs >= kGapMs) {/  } else if (false) {/" $RT
runm tq2_lobby_ignored   $R "s/return switches.lobby ? Place::kStepDown : Place::kOther;/return switches.lobby || !switches.lobby ? Place::kStepDown : Place::kOther;/" $RT
runm tq2_place_unsettled $R "s/  const bool settled = nowMs - gate.placeSinceMs >= kSettleMs \&\& /  const bool settled = /" $RT
# the Resize hold
runm tq2_no_uploads      $R "s/  if (in.uploads != 0) return Hold::kUploads;//" $RT
runm tq2_no_lock         $R "s/  if (in.lock != 0) return Hold::kLock;//" $RT
runm tq2_no_loading      $R "s/  if (in.look.loading) return Hold::kLoading;//" $RT
runm tq2_no_stale        $R "s/  if (!Fresh(in.look, in.nowMs)) return Hold::kStaleLook;//" $RT
runm tq2_no_settling     $R "s/  if (in.quietSinceMs == 0 || in.nowMs - in.quietSinceMs < kSettleMs) return Hold::kSettling;//" $RT
runm tq2_tick_load_unused $R "s/  if (in.look.lastLoadMs != 0 \&\& in.nowMs - in.look.lastLoadMs < kSettleMs) return Hold::kSettling;.*//" $RT
# the owner: who chose, the hold, the look turned off
M=src/quest/sentinel/texture_menu_hook.cpp
MT="quest-host-hooks texture_menu_hook_test"
runm tq2_menu_choice     $M "s/      tq::NotePlayerChose();  \/\/ a change in the menu/      (void)0;  \/\/ a change in the menu/" $MT
runm tq2_ref_choice      $M "s/  tq::NotePlayerChose();  \/\/ the reference rule named a level/  (void)0;  \/\/ the reference rule named a level/" $MT
runm tq2_prop_choice     $M "s/        tq::NotePlayerChose();  \/\/ a level set by hand/        (void)0;  \/\/ a level set by hand/" $MT
runm tq2_hold_skipped    $M "s/  if (lookDriven) {\$/  if (false) {/" $MT
runm tq2_way_back_free   $M "s/(g_lookApplied.load(std::memory_order_acquire) \&\& effective.reason != rules::RuleReason::kPlayerChoice)/false/" $MT
runm tq2_fail_keeps_look $M "s/  if (lookDriven) LookOff(\"resize_failed\");/  (void)0;/" $MT
runm tq2_words_ignored   $M "s/    LookOff(\"upload_words_unavailable\");/    (void)0;/" $MT
# Not a mutant: LookHold's 1U for a null upload word is a null guard only. Unpinned words turn the look off at the poller's first pass
# (tq2_words_ignored), before the gate can turn on, so no look-driven change reaches LookHold with a null word.
runm tq2_gate_unpublished $M "s/  g_lookOn.store(g_gate.on, std::memory_order_release);/  (void)0;/" $MT
runm tq2_held_every_frame $M "s/  if (!g_holding.exchange(true, std::memory_order_acq_rel)) g_lookHolds/  g_holding.store(true); g_lookHolds/" $MT
runm tq2_rule_line_debug $M "s/LogLevel::kInfo, \"texture_quality_level_rule\", fields, 8U/LogLevel::kDebug, \"texture_quality_level_rule\", fields, 8U/" $MT
runm tq2_count_never      $M "s/  if (g_logged.ruleLines >= kRuleLines \&\& g_logged.ruleUnlogged != g_logged.ruleCounted \&\&/  if (false \&\& g_logged.ruleUnlogged != g_logged.ruleCounted \&\&/" $MT
runm tq2_count_every_poll $M "s/(g_logged.ruleCountAtMs == 0 || now - g_logged.ruleCountAtMs >= kRuleCountEveryMs)/true/" $MT
runm tq2_count_when_same  $M "s/g_logged.ruleUnlogged != g_logged.ruleCounted \&\&/true \&\&/" $MT
Q=src/quest/sentinel/texture_quality_hook.cpp
runm tq2_config_choice   $Q "s/  if (fromConfig \&\& level != Level::kStock) NotePlayerChose();//" quest-host-hooks texture_quality_hook_test
runm tq2_config_stock    $Q "s/  if (fromConfig \&\& level != Level::kStock) NotePlayerChose();/  if (fromConfig) NotePlayerChose();/" quest-host-hooks texture_quality_hook_test
# combat's look
C=src/quest/combat/texture_quest2.cpp
runm tq2_lastload_unstored $C "s/  if (loading) g_lastLoadMs.store(now, std::memory_order_release);.*//"
runm tq2_lastload_unread   $C "s/  look.lastLoadMs = g_lastLoadMs.load(std::memory_order_relaxed);//"
runm tq2_key_ignored     $C "s/  if (!on) {/  if (!on \&\& false) {/"
runm tq2_unreadable_ok   $C "s/  const bool loading = !ok || fibers != 0;/  const bool loading = ok \&\& fibers != 0;/"
runm tq2_fibers_ignored  $C "s/  const bool loading = !ok || fibers != 0;/  const bool loading = !ok;/"
runm tq2_not_registered  $C "s/  input::RegisterStepDown(\&CurrentLook, switches);/  (void)switches;/"
runm tq2_arena_loses     $C "/  if (loaded.echoArena) return rules::Where::kEchoArena;/d"
runm tq2_tick_unchecked  $C "s/  if (!tick::Installed()) return Refused(\"tick_not_installed\", lobby);//"
runm tq2_every_frame     $C "s/  if (now < g_nextLookMs.load(std::memory_order_relaxed)) return;//"
runm tq2_lobby_dropped   $C "s/  switches.lobby = lobby;/  switches.lobby = true;/"
# The keys (shared files: run only while holding the port2 shared lock), against quest_config_test.
QC=src/quest/sentinel/quest_config.cpp
runm tq2_qc_default_on  $QC "s/ReadCombatSwitch(cfg, kCombatTextureQuest2StepDownKey, r, false);/ReadCombatSwitch(cfg, kCombatTextureQuest2StepDownKey, r);/" quest-host-shared quest_config_test
runm tq2_qc_lobby_off   $QC "s/ReadCombatSwitch(cfg, kCombatTextureQuest2LobbyKey, r);/ReadCombatSwitch(cfg, kCombatTextureQuest2LobbyKey, r, false);/" quest-host-shared quest_config_test
runm tq2_nc_known_key   src/core/nevr_config.cpp "s/\"texture_quest2_step_down\", \"texture_quest2_lobby\", //" quest-host-shared quest_config_test
