# sendguard: each rule of src/quest/combat/sendguard.cpp and sendguard_targets.h removed or bent once; src/quest/tests/sendguard_test.cpp
# must fail on every one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed>`.
S=src/quest/combat/sendguard.cpp
T=src/quest/combat/sendguard_targets.h
H=src/quest/combat/sendguard.h
# the decision: a named level the lookup does not find is dropped (the null-space drop), and only then
runm sg_drop_inverted      $S "s/return lookup(space, data\[0\]) == nullptr;/return lookup(space, data[0]) != nullptr;/"
runm sg_never_drops        $S "s/return lookup(space, data\[0\]) == nullptr;/return lookup(space, data[0]) == nullptr \&\& false;/"
runm sg_always_drops       $S "s/return lookup(space, data\[0\]) == nullptr;/return lookup(space, data[0]) == nullptr || true;/"
runm sg_drop_calls_stock   $S "/^    LogDrop(node);\$/{n;s/    return;/    original(context, data);/}"
# the level key (data[0]): the lookup is asked it, and -1 means the current space
runm sg_level_key_event    $S "s/return lookup(space, data\[0\]) == nullptr;/return lookup(space, data[1]) == nullptr;/"
runm sg_no_level_looked_up $S "s/  if (data == nullptr || data\[0\] == kNoLevel) return false;/  if (data == nullptr) return false;/"
runm sg_no_level_inverted  $S "s/  if (data == nullptr || data\[0\] == kNoLevel) return false;/  if (data == nullptr || data[0] != kNoLevel) return false;/"
runm sg_no_level_value     $S "s/kNoLevel = ~0ULL;/kNoLevel = ~0ULL - 1;/"
# the chain to the current space: each offset the stock function reads
runm sg_chain_offset       $S "s/kSpaceChain = 0x30;/kSpaceChain = 0x38;/"
runm sg_space_offset       $S "s/kSpaceOffset = 0x80;/kSpaceOffset = 0x88;/"
runm sg_chain_first_load   $S "s/Load(Load(static_cast<const unsigned char\*>(context), 0), 0);/Load(static_cast<const unsigned char*>(context), 0);/"
runm sg_chain_skips_second $S "s/x9 = Load(Load(x9, kSpaceChain), 0);/x9 = Load(x9, kSpaceChain);/"
# a null on the way, a null context or a null node is left to the stock function
runm sg_null_node          $S "s/  if (data == nullptr || data\[0\] == kNoLevel) return false;/  if (data\[0\] == kNoLevel) return false;/"
runm sg_null_chain         $S "s/  if (x9 == nullptr) return false;/  (void)x9;/"
# the original is called with the node's own arguments, and passes are counted
runm sg_arg_swap           $S "s/  original(context, data);/  original(const_cast<void*>(data), context);/"
runm sg_arg_null_data      $S "s/  original(context, data);/  original(context, nullptr);/"
runm sg_pass_not_counted   $S "s/const unsigned n = g_passed.fetch_add(1, std::memory_order_relaxed) + 1;/const unsigned n = g_passed.fetch_add(0, std::memory_order_relaxed) + 1;/"
# the log: every drop counted, the first kDropLines written, each key named
runm sg_drop_not_counted   $S "s/const unsigned n = g_dropped.fetch_add(1, std::memory_order_relaxed) + 1;/const unsigned n = g_dropped.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm sg_drop_lines_cap     $H "s/kDropLines = 8;/kDropLines = 9;/"
runm sg_drop_no_cap        $S "s/  if (n > kDropLines) {/  if (false \&\& n > kDropLines) {/"
runm sg_drop_level_key     $S "s/sentinel::HexString(level, data\[0\])/sentinel::HexString(level, data[1])/"
runm sg_drop_event_key     $S "s/sentinel::HexString(event, data\[1\])/sentinel::HexString(event, data[2])/"
runm sg_drop_actor_key     $S "s/sentinel::HexString(actor, data\[2\])/sentinel::HexString(actor, data[0])/"
runm sg_drop_level_info    $S "s/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_sendguard_dropped\"/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_sendguard_dropped\"/"
# the install: the slot must hold Enter itself, the pins gate it, the build ID gates it
runm sg_no_expected_word   $S "s/  target.expectedOriginal = reinterpret_cast<const void\*>(image.base + kEnterVaddr);/  (void)kEnterVaddr;/"
runm sg_expected_wrong     $S "s/image.base + kEnterVaddr);/image.base + kEnterVaddr + 4);/"
runm sg_pins_skipped       $S "s/if (const char\* const mismatch = FirstPinMismatch(base))/if (const char* const mismatch = (FirstPinMismatch(base), nullptr))/"
runm sg_last_pin_skipped   $S "s/for (std::size_t i = 0; i < kCodeCount; ++i) {/for (std::size_t i = 0; i + 1 < kCodeCount; ++i) {/"
runm sg_pin_words_cut      $S "s/pin.words \* sizeof(std::uint32_t)) != 0) return pin.name;/sizeof(std::uint32_t)) != 0) return pin.name;/"
runm sg_pin_name_lost      $S "s/return pin.name;/return \"pin\";/"
runm sg_build_id_unchecked $S "s/  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\", nullptr);//"
runm sg_no_build_id_ok     $S "s/  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\", nullptr);//"
runm sg_refusal_quiet      $S "s/ModuleResult Refused(const char\* why, const char\* pin) noexcept {/ModuleResult Refused(const char* why, const char* pin) noexcept {\n  return {false, why};/"
runm sg_refused_reports_ok $S "s/  return {installed, installed ? \"installed\" : sentinel::GotStatusName(status)};/  return {true, \"installed\"};/"
runm sg_refused_installed  $S "s/  return {false, why};/  return {true, why};/"
runm sg_install_level      $S "s/sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_sendguard_install\",/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_sendguard_install\",/"
# the target: the symbol, the slot, the pins' words
runm sg_slot_vaddr         $T "s/kEnterSlotVaddr = 0x36f7928ULL;/kEnterSlotVaddr = 0x36f7930ULL;/"
runm sg_unwind_as_slot     $T "s/kEnterSlotVaddr = 0x36f7928ULL;/kEnterSlotVaddr = 0x356a6b0ULL;/"
runm sg_enter_vaddr        $T "s/kEnterVaddr = 0x2725a80ULL;/kEnterVaddr = 0x2725a84ULL;/"
runm sg_stub_vaddr         $T "s/kGameSpaceMtStubVaddr = 0xef5650ULL;/kGameSpaceMtStubVaddr = 0xef5654ULL;/"
runm sg_symbol             $T "s/CSendEventNode5Enter/CSendEventNode5Leave/"
runm sg_pin_word           $T "s/{0xf9404133, 0, 0, 0, 0, 0}, 1}/{0xf9404134, 0, 0, 0, 0, 0}, 1}/"
runm sg_pin_stub_word      $T "s/{0xb0013ef0, 0xf9410a11, 0x91084210, 0xd61f0220, 0, 0}, 4}/{0xb0013ef0, 0xf9410a11, 0x91084210, 0xd61f0221, 0, 0}, 4}/"
runm sg_pin_row_dropped    $T "s/    {\"Enter: bl GameSpaceMT\", 0x2725abcULL, {0x979f3ee5, 0, 0, 0, 0, 0}, 1},//"
# the switch and the old switch file
runm sg_off_ignored        $S "s/  if (!enabled) {/  if (!enabled \&\& false) {/"
runm sg_off_inverted       $S "s/  if (!enabled) {/  if (enabled) {/"
runm sg_off_not_logged     $S "s/{\"result\", \"off\"},/{\"result\", \"on\"},/"
runm sg_legacy_not_told    $S "s/  if (LegacySwitchPresent(legacySwitchDir)) {/  if (false \&\& LegacySwitchPresent(legacySwitchDir)) {/"
runm sg_legacy_always      $S "s/ \&\& access(path, F_OK) == 0;/;/"
runm sg_legacy_turns_off   $S "s/  if (!enabled) {/  if (!enabled || LegacySwitchPresent(legacySwitchDir)) {/"
runm sg_legacy_quiet       $S "s/sentinel::LogLevel::kWarn, \"combat_sendguard_legacy_switch\"/sentinel::LogLevel::kInfo, \"combat_sendguard_legacy_switch\"/"
# the counts: a Debug line every kStatsEvery passes and kStatsDropEvery drops, one Info line at the end when the wrapper is installed
runm sg_stats_pass_never   $S "s/  if ((n \& (kStatsEvery - 1)) == 0) LogStats(false);/  (void)n;/"
runm sg_stats_pass_early   $S "s/  if ((n \& (kStatsEvery - 1)) == 0) LogStats(false);/  if ((n \& (kStatsEvery \/ 2 - 1)) == 0) LogStats(false);/"
runm sg_stats_drop_never   $S "s/    if ((n \& (kStatsDropEvery - 1)) == 0) LogStats(false);  \/\/ the drops after the eighth leave a trace/    (void)n;/"
runm sg_stats_final_flag   $S "s/{\"final\", final ? 1 : 0},/{\"final\", 0},/"
runm sg_stats_level        $S "s/final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, \"combat_sendguard_stats\"/sentinel::LogLevel::kInfo, \"combat_sendguard_stats\"/"
runm sg_stats_passed_field $S "s/{{\"passed\", g_passed.load(std::memory_order_relaxed)},/{{\"passed\", g_dropped.load(std::memory_order_relaxed)},/"
runm sg_end_always         $S "s/  if (g_hook.installed()) LogStats(true);/  LogStats(true);/"
runm sg_end_never          $S "s/  if (g_hook.installed()) LogStats(true);/  (void)g_hook;/"
# off is its own answer; the refused slot leaves the handler off; the old switch file is looked for below the files folder
runm sg_off_is_installed   $S "s/    return {false, \"off_config\", true};/    return {true, \"off_config\", true};/"
runm sg_off_not_off        $S "s/    return {false, \"off_config\", true};/    return {false, \"off_config\", false};/"
runm sg_refused_stays_armed $S "s/  if (!installed \&\& !g_hook.installed()) EnterThunk::Disarm();/  (void)g_hook;/"
runm sg_legacy_subdir      $H "s/kLegacySwitchSubdir = \"q15t\";/kLegacySwitchSubdir = \"q15x\";/"
runm sg_legacy_not_looked  $S "s/fits ? dir : nullptr);/fits ? nullptr : nullptr);/"
# the shared files this module changed (each built and run in its own test)
CI=src/quest/combat/combat_install.cpp
runm ci_off_as_installed   $CI "s/    if (result.off) {/    if (false) {/" quest-host-combat combat_install_test
runm ci_off_not_in_line    $CI "s/{\"failed\", outcome.failed}, {\"off\", outcome.off},/{\"failed\", outcome.failed},/" quest-host-combat combat_install_test
runm ci_off_no_message     $CI "s/    if (outcome.off != 0) std::snprintf/    if (false) std::snprintf/" quest-host-combat combat_install_test
runm ci_end_before_step    $CI "s/ || StateOf(Phase::kNormal).load(std::memory_order_acquire) != kDone) return;  \/\/ nothing was installed/) return;/" quest-host-combat combat_install_test
runm ci_end_when_inactive  $CI "s/  if (!Active() || StateOf(Phase::kNormal)/  if (StateOf(Phase::kNormal)/" quest-host-combat combat_install_test
runm ci_end_not_called     $CI "s/    if (table\[i\].end != nullptr) table\[i\].end();/    (void)table;/" quest-host-combat combat_install_test
runm ci_row_end_null       $CI "s/&nevr_quest::CombatConfig::sendguard, \&sendguard::NoteEnd}/\&nevr_quest::CombatConfig::sendguard, nullptr}/" quest-host-combat combat_install_test
runm ci_row_early          $CI "s/Module{\"sendguard\", nullptr, \&SendguardNormal,/Module{\"sendguard\", \&SendguardNormal, nullptr,/" quest-host-combat combat_install_test
runm ci_row_key_typo       $CI "s/\"combat.sendguard\", \&nevr_quest::CombatConfig::sendguard/\"combat.sendguards\", \&nevr_quest::CombatConfig::sendguard/" quest-host-combat combat_install_test
runm ci_row_no_record      $CI "s/\"combat.sendguard\", \&nevr_quest::CombatConfig::sendguard, /nullptr, nullptr, /" quest-host-combat combat_install_test
QC=src/quest/sentinel/quest_config.cpp
runm qc_default_off        $QC "s/const bool on = ReadBool(cfg, key, fallback, r);/const bool on = ReadBool(cfg, key, !fallback, r);/" quest-host-shared quest_config_test
runm qc_key_typo           $QC "s/kCombatSendguardKey = \"combat.sendguard\";/kCombatSendguardKey = \"combat.sendguards\";/" quest-host-shared quest_config_test
runm qc_not_assigned       $QC "s/  r.config.combat.sendguard = ReadCombatSwitch(cfg, kCombatSendguardKey, r);/  (void)ReadCombatSwitch(cfg, kCombatSendguardKey, r);/" quest-host-shared quest_config_test
runm qc_source_absent      $QC "s/return {on, given ? Source::kFile : Source::kAbsent};/return {on, given ? Source::kAbsent : Source::kAbsent};/" quest-host-shared quest_config_test
runm qc_no_kind_warn       $QC "s/  if (!cfg.GetString(key) \&\& cfg.Kind(key) != nevr::NodeKind::kAbsent) {/  if (false) {/" quest-host-shared quest_config_test
runm nc_known_key          src/core/nevr_config.cpp "s/\"gate_link_libs\", \"sendguard\"}},/\"gate_link_libs\"}},/" quest-host-shared quest_config_test
runm ci_row_ignores_switch  $CI "s/sendguard::Install(g_config.sendguard.on)/sendguard::Install(true)/" quest-host-combat combat_install_test
runm ci_off_counts_heaps    src/quest/combat/heaps.cpp "s/    return {false, \"off\", true};/    return {false, \"off\", false};/" quest-host-combat combat_install_test
runm ci_off_counts_memsys   src/quest/combat/memsys.cpp "s/    return {false, \"off\", true};/    return {false, \"off\", false};/" quest-host-combat heap_step_test
# the process end over the shipped table: called, and called once
runm ci_end_call_dropped   $CI "s/^void NoteProcessEnd() noexcept { RunEnd(kModules.data(), kModules.size()); }/void NoteProcessEnd() noexcept {}/" quest-host-combat sendguard_test
runm ci_end_call_twice     $CI "s/^void NoteProcessEnd() noexcept { RunEnd(kModules.data(), kModules.size()); }/void NoteProcessEnd() noexcept { RunEnd(kModules.data(), kModules.size()); RunEnd(kModules.data(), kModules.size()); }/" quest-host-combat sendguard_test
