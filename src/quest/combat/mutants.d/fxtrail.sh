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
# (the offsets kOffIndex, kOffData, kDataStride and kOffInstance are tied to the pinned words by static_assert in fxtrail_targets.h: a changed one fails the build)
runm fx_slot_narrow        $F "s/SafeRead<std::uint16_t>(index/SafeRead<std::uint8_t>(index/"
# the scope: only a combat map
runm fx_scope_ignored      $F "s/  if (!scope::SeenCombatMap()) return original(cs, handle);/  if (false) return original(cs, handle);/"
runm fx_scope_inverted     $F "s/  if (!scope::SeenCombatMap()) return original(cs, handle);/  if (scope::SeenCombatMap()) return original(cs, handle);/"
runm fx_scope_lobby        $F "s/  if (!scope::SeenCombatMap()) return original(cs, handle);/  if (!scope::SeenCombatOrLobby()) return original(cs, handle);/"
runm fx_scope_after_read   $F "s/  if (!scope::SeenCombatMap()) return original(cs, handle);//"
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
# the install: the slot must hold EffectTime itself, the pins gate it, the build ID gates it, the scope's tick is needed
runm fx_no_expected_word   $F "s/  target.expectedOriginal = reinterpret_cast<const void\*>(image.base + pins.fn);/  (void)pins.fn;/"
runm fx_expected_wrong     $F "s/image.base + pins.fn);/image.base + pins.fn + 4);/"
runm fx_pins_skipped        $F "s/  if (pinned::FirstFailure(image.base, \&kWordPins, 1, \&word)) return Refused(\"pinned_word_mismatch\", \&word);/  if (false) return Refused(\"pinned_word_mismatch\", \&word);/"
runm fx_last_pin_skipped    $T "s/kWordPins = {kWords, kWordCount, nullptr, 0};/kWordPins = {kWords, kWordCount - 1, nullptr, 0};/"
runm fx_pin_unreadable_ok  $F "s/word->readable ? sentinel::HexString(haveText, word->have) : \"unreadable\"/sentinel::HexString(haveText, word->have)/"
runm fx_build_id_skipped    $F "s/  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\");//"
runm fx_no_build_id_ok     $F "s/  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\");//"
runm fx_refusal_quiet      $F "s/sentinel::LogFields(sentinel::LogLevel::kError, \"combat_fxtrail_install\",/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_fxtrail_install\",/"
runm fx_install_level      $F "s/sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_fxtrail_install\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_fxtrail_install\"/"
# the switch and the legacy file
runm fx_off_ignored        $F "s/  if (!on) {/  if (!on \&\& false) {/"
runm fx_off_not_off        $F "s/    return {false, \"off_config\", true};/    return {false, \"off_config\", false};/"
runm fx_legacy_not_told    $F "s/  if (legacy::Present(\"fxtrail.off\")) {/  if (legacy::Present(\"fxtrail.off\") \&\& false) {/"
runm fx_legacy_wrong_file  $F "s/legacy::Present(\"fxtrail.off\")/legacy::Present(\"fxtrail.on\")/"
runm fx_legacy_turns_off   $F "s/  if (!on) {/  if (!on || legacy::Present(\"fxtrail.off\")) {/"
runm fx_legacy_quiet       $F "s/sentinel::LogLevel::kWarn, \"combat_fxtrail_legacy_switch\"/sentinel::LogLevel::kInfo, \"combat_fxtrail_legacy_switch\"/"
runm fx_legacy_no_key      $F "s/{\"key\", \"combat.fxtrail\"}, //"
# the table row (combat_install.cpp): the switch is read, in the normal phase, with an end step
CI=src/quest/combat/combat_install.cpp
runm fx_row_ignores_switch $CI "s/fxtrail::Install(g_config.fxtrail.on)/fxtrail::Install(true)/" quest-host-combat combat_install_test
runm fx_row_end_null       $CI "s/fxtrail, \&fxtrail::NoteEnd}/fxtrail, nullptr}/" quest-host-combat combat_install_test
runm fx_row_early          $CI "s/Module{\"fxtrail\", nullptr, \&FxtrailNormal,/Module{\"fxtrail\", \&FxtrailNormal, nullptr,/" quest-host-combat combat_install_test
# the key
QC=src/quest/sentinel/quest_config.cpp
runm fx_qc_default_off     $QC "s/  r.config.combat.fxtrail = ReadCombatSwitch(cfg, kCombatFxtrailKey, r);/  r.config.combat.fxtrail = ReadCombatSwitch(cfg, kCombatFxtrailKey, r, false);/" quest-host-shared quest_config_test
runm fx_qc_key_typo        $QC "s/kCombatFxtrailKey = \"combat.fxtrail\";/kCombatFxtrailKey = \"combat.fxtrails\";/" quest-host-shared quest_config_test
runm fx_qc_not_assigned    $QC "s/  r.config.combat.fxtrail = ReadCombatSwitch(cfg, kCombatFxtrailKey, r);/  (void)ReadCombatSwitch(cfg, kCombatFxtrailKey, r);/" quest-host-shared quest_config_test
runm fx_nc_known_key       src/core/nevr_config.cpp "s/\"shieldgamma_tube_mirror\", \"fxtrail\", /\"shieldgamma_tube_mirror\", /" quest-host-shared quest_config_test

# arena pays nothing, by count (the test counts SafeRead calls and level scans on every arena level): a read or a scan before the scope test is killed
runm fx_reads_before_scope $F "s/^  if (!scope::SeenCombatMap()) return original(cs, handle);/  { bool okk = true; (void)SafeRead<std::uint32_t>(reinterpret_cast<std::uintptr_t>(cs), \&okk); }  if (!scope::SeenCombatMap()) return original(cs, handle);/"
runm fx_scan_before_scope  $F "s/^#include \"quest\/combat\/safe_read.h\"/#include \"quest\/combat\/safe_read.h\"\n#include \"quest\/combat\/combat_level.h\"/; s/^  if (!scope::SeenCombatMap()) return original(cs, handle);/  (void)level::Scan(reinterpret_cast<std::uintptr_t>(cs));  if (!scope::SeenCombatMap()) return original(cs, handle);/"
# a true answer older than kStaleAfterMs is not believed
runm fx_stale_believed     $F "s/  if (!scope::LookIsFresh()) return time;.*/  (void)time;/"
# the scope must be able to change, or the hook is not installed
runm fx_live_unchecked     $F "s/  if (!scope::Live()) return Refused(\"scope_not_live\");//"
runm fx_live_after_hook    $F "s/  if (!scope::Live()) return Refused(\"scope_not_live\");/  (void)scope::Live();/"
runm fx_live_before_off    $F "s/  if (!on) {/  if (!scope::Live()) return Refused(\"scope_not_live\");\n  if (!on) {/"
# the scope row is on while this guard is (combat_install.cpp ScopeNormal)
runm fx_reader_unlisted    $CI "s/ || g_config.fxtrail.on//" quest-host-combat combat_install_test
runm fx_row_member         $CI "s/\"combat.fxtrail\", \&nevr_quest::CombatConfig::fxtrail,/\"combat.fxtrail\", \&nevr_quest::CombatConfig::voipdrop,/" quest-host-combat combat_install_test
runm fx_row_key            $CI "s/\"combat.fxtrail\", \&nevr_quest/\"combat.fxtrails\", \&nevr_quest/" quest-host-combat combat_install_test
# arena pays nothing, by count (the test counts SafeRead calls and level scans on every arena level): a read or a scan before the scope test is killed
# a true answer older than kStaleAfterMs is not believed
# the scope must be able to change, or the hook is not installed
# the scope row is on while this guard is (combat_install.cpp ScopeNormal)