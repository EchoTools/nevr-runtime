# fxtrail: each rule of src/quest/combat/fxtrail.cpp and fxtrail_targets.h removed or bent once; src/quest/tests/fxtrail_test.cpp must fail on every
# one (the shared files it changed are each run against the test that covers them). Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
F=src/quest/combat/fxtrail.cpp
T=src/quest/combat/fxtrail_targets.h
H=src/quest/combat/fxtrail.h
# the decision: no instance, in a combat map, ends the trail; the game's value is returned otherwise
runm fx_no_inst_check      $F "s/  if (!ok || instance != 0) return time;/  if (!ok || (instance \& 0) != 0) return time;/"
runm fx_never_ends         $F "s/  if (!ok || instance != 0) return time;/  if (!ok || instance != 0 || true) return time;/"
runm fx_unread_ends        $F "s/  if (!ok || instance != 0) return time;/  if (instance != 0) return time;/"
runm fx_value_changed      $F "s/  if (!ok || instance != 0) return time;/  if (!ok || instance != 0) return time + 1.0f;/"
runm fx_ends_with_zero     $F "s/  return FLT_MAX;/  return 0.0f;/"
runm fx_no_original        $F "s/  const float time = original(cs, handle);/  const float time = 0.0f; (void)original;/"
runm fx_original_args      $F "s/  const float time = original(cs, handle);/  const float time = original(cs, handle + 1);/"
# the lookup the game's function makes
runm fx_no_mask            $F "s/index + (handle \& 0xffff) \* 4/index + handle * 4/"
runm fx_index_stride       $F "s/index + (handle \& 0xffff) \* 4/index + (handle \& 0xffff) * 2/"
runm fx_data_stride        $T "s/kDataStride = 0x18/kDataStride = 0x10/"
runm fx_instance_off       $T "s/kOffInstance = 0x10/kOffInstance = 0x08/"
runm fx_index_off          $T "s/kOffIndex = 0xc8/kOffIndex = 0xc0/"
runm fx_data_off           $T "s/kOffData = 0xf8/kOffData = 0xf0/"
runm fx_slot_narrow        $F "s/SafeRead<std::uint16_t>(index/SafeRead<std::uint8_t>(index/"
# the scope: only a combat map
runm fx_scope_ignored      $F "s/  if (!scope::InCombatMap()) return time;/  if (false) return time;/"
runm fx_scope_inverted     $F "s/  if (!scope::InCombatMap()) return time;/  if (scope::InCombatMap()) return time;/"
runm fx_scope_lobby        $F "s/  if (!scope::InCombatMap()) return time;/  if (!scope::InCombatMapOrLobby()) return time;/"
runm fx_scope_after_read   $F "s/  if (!scope::InCombatMap()) return time;//"
# the counters and the lines
runm fx_not_in_combat_counted $F "s/g_inCombat.fetch_add(1, std::memory_order_relaxed) + 1;/g_inCombat.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm fx_ended_not_counted  $F "s/g_ended.fetch_add(1, std::memory_order_relaxed) + 1;/g_ended.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm fx_log_every          $F "s/  if ((n \& (n - 1)) == 0) LogEnded(n);/  LogEnded(n);/"
runm fx_log_never          $F "s/  if ((n \& (n - 1)) == 0) LogEnded(n);/  if (false) LogEnded(n);/"
runm fx_ended_level        $F "s/sentinel::LogLevel::kInfo, \"combat_fxtrail_ended\"/sentinel::LogLevel::kDebug, \"combat_fxtrail_ended\"/"
runm fx_stats_never        $F "s/  if ((inCombat \& (kStatsEvery - 1)) == 0) LogStats(false);/  (void)inCombat;/"
runm fx_stats_every        $F "s/  if ((inCombat \& (kStatsEvery - 1)) == 0) LogStats(false);/  if (inCombat != 0) LogStats(false);/"
runm fx_stats_every_wrong  $H "s/kStatsEvery = 16384;/kStatsEvery = 8192;/"
runm fx_stats_final_never  $F "s/  if (g_hook.installed()) LogStats(true);/  (void)g_hook;/"
runm fx_stats_final_always $F "s/  if (g_hook.installed()) LogStats(true);/  LogStats(true);/"
runm fx_stats_final_debug  $F "s/final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, \"combat_fxtrail_stats\"/sentinel::LogLevel::kDebug, \"combat_fxtrail_stats\"/"
runm fx_stats_looks        $F "s/{\"level_looks\", scope::Looks()}/{\"level_looks\", 0}/"
# the install: the slot must hold EffectTime itself, the pins gate it, the build ID gates it, the scope's tick is needed
runm fx_no_expected_word   $F "s/  target.expectedOriginal = reinterpret_cast<const void\*>(image.base + pins.fn);/  (void)pins.fn;/"
runm fx_expected_wrong     $F "s/image.base + pins.fn);/image.base + pins.fn + 4);/"
runm fx_pins_skipped       $F "s/  if (bad != kWordCount) {/  if (false) {/"
runm fx_last_pin_skipped   $F "s/FirstWordMismatch(image.base, kWords, kWordCount, \&have, \&readable)/FirstWordMismatch(image.base, kWords, kWordCount - 1, \&have, \&readable)/"
runm fx_pin_unreadable_ok  $F "s/readable ? sentinel::HexString(haveText, have) : \"unreadable\"/sentinel::HexString(haveText, have)/"
runm fx_build_id_skipped   $F "s/  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\");//"
runm fx_no_build_id_ok     $F "s/  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\");//"
runm fx_no_scope_start     $F "s/  if (!scope::Start()) return Refused(\"tick_table_full\");.*/  (void)scope::Start();/"
runm fx_scope_start_late   $F "s/  if (!scope::Start()) return Refused(\"tick_table_full\");.*//"
runm fx_refusal_quiet      $F "s/sentinel::LogFields(sentinel::LogLevel::kError, \"combat_fxtrail_install\",/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_fxtrail_install\",/"
runm fx_install_level      $F "s/sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_fxtrail_install\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_fxtrail_install\"/"
# the switch and the legacy file
runm fx_off_ignored        $F "s/  if (!on) {/  if (!on \&\& false) {/"
runm fx_off_not_off        $F "s/    return {false, \"off_config\", true};/    return {false, \"off_config\", false};/"
runm fx_off_installs_tick  $F "s/    return {false, \"off_config\", true};/    scope::Start(); return {false, \"off_config\", true};/"
runm fx_legacy_not_told    $F "s/  if (LegacySwitchFilePresent(legacyDir, \"fxtrail.off\")) {/  if (LegacySwitchFilePresent(legacyDir, \"fxtrail.off\") \&\& false) {/"
runm fx_legacy_wrong_file  $F "s/LegacySwitchFilePresent(legacyDir, \"fxtrail.off\")/LegacySwitchFilePresent(legacyDir, \"fxtrail.on\")/"
runm fx_legacy_turns_off   $F "s/  if (!on) {/  if (!on || LegacySwitchFilePresent(legacyDir, \"fxtrail.off\")) {/"
runm fx_legacy_quiet       $F "s/sentinel::LogLevel::kWarn, \"combat_fxtrail_legacy_switch\"/sentinel::LogLevel::kInfo, \"combat_fxtrail_legacy_switch\"/"
runm fx_legacy_no_key      $F "s/{\"key\", \"combat.fxtrail\"}, //"
# the table row (combat_install.cpp): the switch is read, in the normal phase, with an end step
CI=src/quest/combat/combat_install.cpp
runm fx_row_ignores_switch $CI "s/fxtrail::Install(g_config.fxtrail.on)/fxtrail::Install(true)/" quest-host-combat combat_install_test
runm fx_row_end_null       $CI "s/\&FxtrailNormal, \&fxtrail::NoteEnd}/\&FxtrailNormal, nullptr}/" quest-host-combat combat_install_test
runm fx_row_early          $CI "s/Module{\"fxtrail\", nullptr, \&FxtrailNormal,/Module{\"fxtrail\", \&FxtrailNormal, nullptr,/" quest-host-combat combat_install_test
# the key
QC=src/quest/sentinel/quest_config.cpp
runm fx_qc_default_off     $QC "s/  r.config.combat.fxtrail = ReadCombatSwitch(cfg, kCombatFxtrailKey, r);/  r.config.combat.fxtrail = ReadCombatSwitch(cfg, kCombatFxtrailKey, r, false);/" quest-host-shared quest_config_test
runm fx_qc_key_typo        $QC "s/kCombatFxtrailKey = \"combat.fxtrail\";/kCombatFxtrailKey = \"combat.fxtrails\";/" quest-host-shared quest_config_test
runm fx_qc_not_assigned    $QC "s/  r.config.combat.fxtrail = ReadCombatSwitch(cfg, kCombatFxtrailKey, r);/  (void)ReadCombatSwitch(cfg, kCombatFxtrailKey, r);/" quest-host-shared quest_config_test
runm fx_nc_known_key       src/core/nevr_config.cpp "s/\"refresh_rate_quest2_72\", \"fxtrail\", /\"refresh_rate_quest2_72\", /" quest-host-shared quest_config_test
# the shared helpers every guard uses: the pinned-word check (pinned_words.h) and the legacy switch file (legacy_switch.h)
PW=src/quest/combat/pinned_words.h
runm sh_pw_never_mismatch  $PW "s/    if (!ok || word != pins\[i\].word) {/    if (false) {/"
runm sh_pw_unreadable_ok   $PW "s/    if (!ok || word != pins\[i\].word) {/    if (ok \&\& word != pins[i].word) {/"
runm sh_pw_readable_flag   $PW "s/      \*readable = ok;/      *readable = true;/"
runm sh_pw_index_off_by_one $PW "s/      return i;/      return i + 1;/"
runm sh_pw_count_ignored   $PW "s/  for (std::size_t i = 0; i < count; ++i) {/  for (std::size_t i = 0; i + 1 < count; ++i) {/"
LS=src/quest/combat/legacy_switch.h
runm sh_ls_always_absent   $LS "s/  return wrote >= 0 /  return false \&\& wrote >= 0 /"
runm sh_ls_no_access       $LS "s/ \&\& access(path, F_OK) == 0;/;/"
runm sh_ls_dir_subdir      $LS "s/kLegacySwitchSubdir = \"q15t\";/kLegacySwitchSubdir = \"q15x\";/"
# the level scope each scoped guard asks is covered by level_scope.sh; the guards' keys line and the shipped table by combat_install_test
runm sh_ci_guards_line_key $CI "s/{\"voipdrop\", config.voipdrop.on ? 1 : 0}, {\"voipdrop_source\", source(config.voipdrop.source)},/{\"voipdrop\", 1}, {\"voipdrop_source\", source(config.voipdrop.source)},/" quest-host-combat combat_install_test
runm sh_ci_guards_line_src $CI "s/{\"peerclamp_source\", source(config.peerclamp.source)}/{\"peerclamp_source\", \"default\"}/" quest-host-combat combat_install_test
runm sh_ci_guards_line_stage1 $CI "s/{\"droneguard_stage1\", config.droneguardStage1.on ? 1 : 0}/{\"droneguard_stage1\", 0}/" quest-host-combat combat_install_test
runm sh_ci_order           $CI "s/    Module{\"fxtrail\", nullptr, \&FxtrailNormal, \&fxtrail::NoteEnd},/    Module{\"fxtrail\", nullptr, \&FxtrailNormal, \&fxtrail::NoteEnd}, Module{\"fxtrail\", nullptr, \&FxtrailNormal, \&fxtrail::NoteEnd},/" quest-host-combat combat_install_test
