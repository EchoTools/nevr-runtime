# pfx: each rule of src/quest/combat/pfx.cpp removed or bent once; src/quest/tests/pfx_test.cpp must fail on every one. Sourced by ../mutants.sh, which
# defines `runm <name> <file> <sed> [<build dir> <test target>]`. Run: src/quest/combat/mutants.sh pfx
F=src/quest/combat/pfx.cpp
# the decision: on unless the key is false, and the reason names which
runm pfx_decide_inverted     $F "s/  if (!configOn) return {false, GateReason::kConfigOff};/  if (configOn) return {false, GateReason::kConfigOff};/"
runm pfx_decide_never_off    $F "s/  if (!configOn) return {false, GateReason::kConfigOff};/  if (configOn \&\& false) return {false, GateReason::kConfigOff};/"
runm pfx_decide_off_is_on    $F "s/  if (!configOn) return {false, GateReason::kConfigOff};/  if (!configOn) return {true, GateReason::kConfigOff};/"
runm pfx_decide_off_reason   $F "s/  if (!configOn) return {false, GateReason::kConfigOff};/  if (!configOn) return {false, GateReason::kDefault};/"
runm pfx_decide_on_reason    $F "s/  return {true, GateReason::kDefault};/  return {true, GateReason::kConfigOff};/"
runm pfx_decide_on_is_off    $F "s/  return {true, GateReason::kDefault};/  return {false, GateReason::kDefault};/"
# the stored decision: what GateOn reads
runm pfx_store_always_on     $F "s/  g_on.store(decision.on, std::memory_order_release);/  g_on.store(true, std::memory_order_release);/"
runm pfx_store_always_off    $F "s/  g_on.store(decision.on, std::memory_order_release);/  g_on.store(false, std::memory_order_release);/"
runm pfx_store_missing       $F "s/  g_on.store(decision.on, std::memory_order_release);/  (void)decision;/"
runm pfx_gateon_const_true   $F "s/bool GateOn() noexcept { return g_on.load(std::memory_order_acquire); }/bool GateOn() noexcept { return true; }/"
runm pfx_gateon_const_false  $F "s/bool GateOn() noexcept { return g_on.load(std::memory_order_acquire); }/bool GateOn() noexcept { return false; }/"
runm pfx_initial_on          $F "s/std::atomic<bool> g_on{false};/std::atomic<bool> g_on{true};/"
runm pfx_reset_keeps         $F "s/^  g_on.store(false, std::memory_order_release);\$/  (void)0;/"
# the install line: Info for a choice, with the state, reason, key and scope
runm pfx_install_level       $F "s/sentinel::LogLevel::kInfo, \"combat_pfx_install\"/sentinel::LogLevel::kDebug, \"combat_pfx_install\"/"
runm pfx_install_off_error   $F "s/sentinel::LogLevel::kInfo, \"combat_pfx_install\"/decision.on ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_pfx_install\"/"
runm pfx_state_inverted      $F "s/{\"state\", decision.on ? \"on\" : \"off\"}/{\"state\", decision.on ? \"off\" : \"on\"}/"
runm pfx_reason_inverted     $F "s/decision.reason == GateReason::kConfigOff ? \"off_config\" : \"default\"/decision.reason != GateReason::kConfigOff ? \"off_config\" : \"default\"/"
runm pfx_reason_off_token    $F "s/GateReason::kConfigOff ? \"off_config\" : \"default\"/GateReason::kConfigOff ? \"config_off\" : \"default\"/"
runm pfx_reason_on_token     $F "s/GateReason::kConfigOff ? \"off_config\" : \"default\"/GateReason::kConfigOff ? \"off_config\" : \"on\"/"
runm pfx_switch_key          $F "s/{\"switch\", \"combat.pfx\"}/{\"switch\", \"combat.pf\"}/"
runm pfx_scope_token         $F "s/{\"scope\", \"combat_map\"}/{\"scope\", \"combat\"}/"
runm pfx_msg_swapped         $F "s/{\"msg\", decision.on ? \"the particle-renderer gate is on/{\"msg\", !decision.on ? \"the particle-renderer gate is on/"
# the plugin's kill file: never read, one Warning when found
runm pfx_legacy_file         $F "s/legacy::Present(\"pfx.off\")/legacy::Present(\"pfx.on\")/"
runm pfx_legacy_always       $F "s/  if (legacy::Present(\"pfx.off\")) {/  if (true) {/"
runm pfx_legacy_never        $F "s/  if (legacy::Present(\"pfx.off\")) {/  if (false) {/"
runm pfx_legacy_level        $F "s/sentinel::LogLevel::kWarn, \"combat_pfx_legacy_switch\"/sentinel::LogLevel::kInfo, \"combat_pfx_legacy_switch\"/"
runm pfx_legacy_file_field   $F "s/{\"file\", \"pfx.off\"}/{\"file\", \"pfx.on\"}/"
runm pfx_legacy_replaced_by  $F "s/{\"replaced_by\", \"combat.pfx: false\"}/{\"replaced_by\", \"combat.pfx: true\"}/"
runm pfx_legacy_msg          $F "s/the plugin's kill file is no longer read and changes nothing;/the plugin's kill file turns the gate off;/"
runm pfx_legacy_turns_off    $F "s/  if (legacy::Present(\"pfx.off\")) {/&\n    g_on.store(false, std::memory_order_release);/"
# the step's result
runm pfx_result_installed    $F "s/  return {decision.on, decision.on ? \"installed\" : \"off_config\"};/  return {true, decision.on ? \"installed\" : \"off_config\"};/"
runm pfx_result_off          $F "s/  return {decision.on, decision.on ? \"installed\" : \"off_config\"};/  return {false, decision.on ? \"installed\" : \"off_config\"};/"
runm pfx_result_status_on    $F "s/decision.on ? \"installed\" : \"off_config\"};/decision.on ? \"on\" : \"off_config\"};/"
runm pfx_result_status_off   $F "s/decision.on ? \"installed\" : \"off_config\"};/decision.on ? \"installed\" : \"off\"};/"
# the level scope: a combat map only, never the combat lobby, never an arena level
runm pfx_lobby_counts        $F "s/constexpr bool kWithCombatLobby = false;/constexpr bool kWithCombatLobby = true;/"
runm pfx_kind_inverted       $F "s/== level::Kind::kCombatOrLobby;/!= level::Kind::kCombatOrLobby;/"
runm pfx_kind_not_arena      $F "s/return level::Classify(game, kWithCombatLobby, \&levelId) == level::Kind::kCombatOrLobby;/return level::Classify(game, kWithCombatLobby, \&levelId) != level::Kind::kEchoArena;/"
runm pfx_classify_game       $F "s/level::Classify(game, kWithCombatLobby, /level::Classify(game \& 0, kWithCombatLobby, /"
runm pfx_open_or             $F "s/return GateOn() \&\& InCombatMap(game);/return GateOn() || InCombatMap(game);/"
runm pfx_open_no_switch      $F "s/return GateOn() \&\& InCombatMap(game);/return InCombatMap(game);/"
runm pfx_open_no_level       $F "s/return GateOn() \&\& InCombatMap(game);/(void)game; return GateOn();/"
runm pfx_open_game_arg       $F "s/return GateOn() \&\& InCombatMap(game);/return GateOn() \&\& InCombatMap(game \& 0);/"
# test support the tests rely on
runm pfx_legacy_dir_noop     $F "s/void SetLegacyDirForTest(const char\* dir) noexcept { legacy::SetDirectoryForTest(dir); }/void SetLegacyDirForTest(const char* dir) noexcept { (void)dir; }/"
