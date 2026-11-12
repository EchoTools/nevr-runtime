# refresh_rate: each rule of src/quest/combat/refresh_rate.cpp removed or bent once; src/quest/tests/refresh_rate_test.cpp must fail on every one.
# The `rrrow_` mutants edit the install table (src/quest/combat/combat_install.cpp, a shared file) and are run against combat_install_test: run them
# apart from the others (`mutants.sh refresh_rate '^rr_'`, then `mutants.sh refresh_rate '^rrrow_'` while holding the shared lock).
# Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target>]`.
#
# Not mutated, because no run can tell the mutant from the original:
#   - `askedHz > rule.levelHz` to `>=` in RateFor: at equality both answer the level rate, which is the asked rate;
#   - `rule.on` and `rule.lobby` in PublishedRule: RateFor reads `rule.on` only together with `g_inLevel`, which the tick sets only while `g_on` is
#     true and nothing clears, and RateFor never reads `rule.lobby`;
#   - `override >= 0` to `> 0` in ReadDeviceIsQuest2: off Android the fall-through answer is also false;
#   - `!target.slotVaddr.has_value()` in AskThroughTheSlot: the pinned slot always has a value;
#   - the test seams (Set*ForTest, ResetForTest) and the static_asserts (a changed constant does not compile).
F=src/quest/combat/refresh_rate.cpp
CI=src/quest/combat/combat_install.cpp
# the rule: three keys and the headset
runm rr_decide_on              $F "s/  rule.on = inputs.rule;/  rule.on = true;/"
runm rr_decide_lobby_no_on     $F "s/  rule.lobby = rule.on \&\& inputs.lobby;/  rule.lobby = inputs.lobby;/"
runm rr_decide_lobby_no_key    $F "s/  rule.lobby = rule.on \&\& inputs.lobby;/  rule.lobby = rule.on;/"
runm rr_decide_lobby_or        $F "s/  rule.lobby = rule.on \&\& inputs.lobby;/  rule.lobby = rule.on || inputs.lobby;/"
runm rr_decide_q2_no_on        $F "s/  rule.levelHz = rule.on \&\& inputs.quest2 \&\& inputs.quest2At72 ?/  rule.levelHz = inputs.quest2 \&\& inputs.quest2At72 ?/"
runm rr_decide_q2_no_headset   $F "s/  rule.levelHz = rule.on \&\& inputs.quest2 \&\& inputs.quest2At72 ?/  rule.levelHz = rule.on \&\& inputs.quest2At72 ?/"
runm rr_decide_q2_no_key       $F "s/  rule.levelHz = rule.on \&\& inputs.quest2 \&\& inputs.quest2At72 ?/  rule.levelHz = rule.on \&\& inputs.quest2 ?/"
runm rr_decide_q2_rates_swapped $F "s/ ? kQuest2LevelHz : kLevelHz;/ ? kLevelHz : kQuest2LevelHz;/"
# the rate for a request
runm rr_rate_no_on             $F "s/  return rule.on \&\& inLevel \&\& askedHz > rule.levelHz ?/  return inLevel \&\& askedHz > rule.levelHz ?/"
runm rr_rate_no_level          $F "s/  return rule.on \&\& inLevel \&\& askedHz > rule.levelHz ?/  return rule.on \&\& (inLevel || true) \&\& askedHz > rule.levelHz ?/"
runm rr_rate_no_above          $F "s/  return rule.on \&\& inLevel \&\& askedHz > rule.levelHz ?/  return rule.on \&\& inLevel ?/"
runm rr_rate_below_raised      $F "s/askedHz > rule.levelHz ?/askedHz < rule.levelHz ?/"
runm rr_rate_else_level        $F "s/rule.levelHz : askedHz;/rule.levelHz : rule.levelHz;/"
# Choose: the release gate, the tick's own re-ask, what is kept for the tick, the level
runm rr_choose_gate            $F "s/^  if (!Active()) return askedHz;.*\$//"
runm rr_choose_own_reask       $F "s/^  if (IsOwnReask()) return askedHz;.*\$//"
runm rr_choose_ovr_kept        $F "s/^  g_ovr.store(ovr, std::memory_order_release);/  (void)ovr;/"
runm rr_choose_session_kept    $F "s/^  g_session.store(askedHz, std::memory_order_release);/  (void)askedHz;/"
runm rr_choose_level_ignored   $F "s/g_inLevel.load(std::memory_order_acquire), askedHz);/true, askedHz);/"
# Note: the gate, the re-ask, the counters and the request line
runm rr_note_no_gate           $F "s/  if (!Active() || IsOwnReask()) return;/  if (IsOwnReask()) return;/"
runm rr_note_counts_reask      $F "s/  if (!Active() || IsOwnReask()) return;/  if (!Active()) return;/"
runm rr_note_uncounted         $F "s/g_requests.fetch_add(1, std::memory_order_relaxed) + 1;/g_requests.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm rr_note_asked_kept        $F "s/  g_asked.store(askedHz, std::memory_order_relaxed);/  (void)askedHz;/"
runm rr_note_sent_kept         $F "s/  g_sent.store(sentHz, std::memory_order_relaxed);/  (void)sentHz;/"
runm rr_note_why_gt            $F "s/sentHz < askedHz ?/sentHz > askedHz ?/"
runm rr_note_why_le            $F "s/sentHz < askedHz ?/sentHz <= askedHz ?/"
runm rr_note_level             $F "s/sentinel::LogLevel::kInfo, \"combat_refresh_rate_request\"/sentinel::LogLevel::kDebug, \"combat_refresh_rate_request\"/"
runm rr_note_n                 $F "/\"combat_refresh_rate_request\"/,/\"mode\"/s/{{\"n\", n},/{{\"n\", n + 1},/"
runm rr_note_asked_text        $F "/\"combat_refresh_rate_request\"/,/\"mode\"/s/{\"asked_hz\", asked.text}/{\"asked_hz\", asked.text[0] ? sent.text : asked.text}/"
runm rr_note_sent_text         $F "/\"combat_refresh_rate_request\"/,/\"mode\"/s/{\"sent_hz\", sent.text}/{\"sent_hz\", sent.text[0] ? asked.text : sent.text}/"
runm rr_note_rc                $F "/\"combat_refresh_rate_request\"/,/\"mode\"/s/{\"rc\", result}/{\"rc\", result * 0}/"
runm rr_note_mode              $F "/\"combat_refresh_rate_request\"/,/\"quest2\"/s/{\"mode\", ModeName()}/{\"mode\", \"match\"}/"
runm rr_note_quest2            $F "/\"combat_refresh_rate_request\"/,/\"quest2\"/s/g_quest2.load(std::memory_order_relaxed) ? 1 : 0/g_quest2.load(std::memory_order_relaxed) ? 0 : 1/"
runm rr_hz_text                $F "s/\"%.1f\", static_cast<double>(hz)/\"%.0f\", static_cast<double>(hz)/"
# Install: the gate, each key and the headset into the rule, what is published, the tick, the line
runm rr_install_gate           $F "s/  if (!Active()) return {false, \"inactive\"};//"
runm rr_install_rule_arg       $F "s/  inputs.rule = rule;/  inputs.rule = rule || true;/"
runm rr_install_lobby_arg      $F "s/  inputs.lobby = lobby;/  inputs.lobby = lobby \&\& false;/"
runm rr_install_q2key_arg      $F "s/  inputs.quest2At72 = quest2At72;/  inputs.quest2At72 = quest2At72 || true;/"
runm rr_install_headset        $F "s/  inputs.quest2 = ReadDeviceIsQuest2();/  inputs.quest2 = ReadDeviceIsQuest2() \&\& false;/"
runm rr_install_q2_kept        $F "s/  g_quest2.store(inputs.quest2, std::memory_order_relaxed);/  (void)inputs;/"
runm rr_install_hz_kept        $F "s/  g_levelHz.store(decided.levelHz, std::memory_order_release);/  (void)decided;/"
runm rr_install_lobby_kept     $F "s/  g_lobby.store(decided.lobby, std::memory_order_release);/  (void)decided;/"
runm rr_install_on_kept        $F "s/  g_on.store(decided.on, std::memory_order_release);.*\$/  (void)decided;/"
runm rr_install_tick_always    $F "s/  if (decided.on) tickState = /  if (true) tickState = /"
runm rr_install_tick_never     $F "s/  if (decided.on) tickState = /  if (false) tickState = /"
runm rr_install_tick_swapped   $F "s/tick::Add(\&Tick) ? \"registered\" : \"full\";/tick::Add(\&Tick) ? \"full\" : \"registered\";/"
runm rr_install_full_cmp       $F "s/std::strcmp(tickState, \"full\") == 0;/std::strcmp(tickState, \"full\") != 0;/"
runm rr_install_hz_text        $F "s/std::snprintf(hz, sizeof(hz), \"%.0f\"/std::snprintf(hz, sizeof(hz), \"%.1f\"/"
runm rr_install_level_info     $F "s/sentinel::LogFields(full ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, \"combat_refresh_rate_install\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_refresh_rate_install\"/"
runm rr_install_level_inverted $F "s/sentinel::LogFields(full ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, \"combat_refresh_rate_install\"/sentinel::LogFields(full ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, \"combat_refresh_rate_install\"/"
runm rr_install_mode           $F "s/{{\"mode\", ModeName()},/{{\"mode\", \"match\"},/"
runm rr_install_rule_field     $F "s/{\"refresh_rate\", inputs.rule ? 1 : 0}/{\"refresh_rate\", inputs.rule ? 0 : 1}/"
runm rr_install_lobby_field    $F "s/{\"refresh_rate_lobby\", inputs.lobby ? 1 : 0}/{\"refresh_rate_lobby\", inputs.lobby ? 0 : 1}/"
runm rr_install_q2key_field    $F "s/{\"refresh_rate_quest2_72\", inputs.quest2At72 ? 1 : 0}/{\"refresh_rate_quest2_72\", inputs.quest2At72 ? 0 : 1}/"
runm rr_install_q2_field       $F "s/{\"quest2\", inputs.quest2 ? 1 : 0}/{\"quest2\", inputs.quest2 ? 0 : 1}/"
runm rr_install_hz_field       $F "s/{\"level_hz\", hz}/{\"level_hz\", \"90\"}/"
runm rr_install_tick_field     $F "s/{\"tick\", tickState}/{\"tick\", \"registered\"}/"
runm rr_install_msg_full       $F "s/{\"msg\", full ? \"the game-thread/{\"msg\", !full ? \"the game-thread/"
runm rr_install_msg_off        $F "s/: !decided.on ? \"refresh_rate is off/: decided.on ? \"refresh_rate is off/"
runm rr_install_full_returns   $F "s/  if (full) return {false, \"tick_table_full\"};//"
runm rr_install_status         $F "s/  return {true, decided.on ? \"installed\" : \"witness\"};/  return {true, decided.on ? \"witness\" : \"installed\"};/"
runm rr_install_no_legacy      $F "s/^  WarnLegacyKillFiles();/  if (false) WarnLegacyKillFiles();/"
# the plugin's kill files: only a Warning, naming the key that replaces each
runm rr_legacy_not_looked      $F "s/    if (!legacy::Present(entry.file)) continue;/    (void)entry;/"
runm rr_legacy_file_level      $F "s/{\"hzlevel.off\", /{\"hzlevel.of\", /"
runm rr_legacy_file_lobby      $F "s/{\"hzlobby90.off\", /{\"hzlobby90.of\", /"
runm rr_legacy_file_q2         $F "s/{\"hz72q2.off\", /{\"hz72q2.of\", /"
runm rr_legacy_key_level       $F "s/\"combat.refresh_rate: false\"/\"combat.refresh_rate: true\"/"
runm rr_legacy_key_lobby       $F "s/(the default)\"/(the default!)\"/"
runm rr_legacy_key_q2          $F "s/\"combat.refresh_rate_quest2_72: false\"/\"combat.refresh_rate_quest2_72: true\"/"
runm rr_legacy_level           $F "s/sentinel::LogLevel::kWarn, \"combat_refresh_rate_legacy_switch\"/sentinel::LogLevel::kInfo, \"combat_refresh_rate_legacy_switch\"/"
runm rr_legacy_file_field      $F "s/{{\"file\", entry.file},/{{\"file\", entry.key},/"
runm rr_legacy_key_field       $F "s/{\"replaced_by\", entry.key},/{\"replaced_by\", entry.file},/"
# the tick: its schedule, what it waits for, the level, the re-ask, the line
runm rr_tick_off_ignored       $F "s/  if (!g_on.load(std::memory_order_acquire)) return;/  (void)g_on;/"
runm rr_tick_every_frame       $F "s/  if (now < g_nextLook) return;//"
runm rr_tick_look_edge         $F "s/  if (now < g_nextLook) return;/  if (now <= g_nextLook) return;/"
runm rr_tick_spacing_short     $F "s/kLookEverySeconds = 0.5;/kLookEverySeconds = 0.4;/"
runm rr_tick_spacing_long      $F "s/kLookEverySeconds = 0.5;/kLookEverySeconds = 0.6;/"
runm rr_tick_next_not_set      $F "s/  g_nextLook = now + kLookEverySeconds;/  g_nextLook = 0.0;/"
runm rr_tick_session_edge      $F "s/  if (session <= low || ovr == nullptr) return;/  if (session < low || ovr == nullptr) return;/"
runm rr_tick_no_ovr_check      $F "s/  if (session <= low || ovr == nullptr) return;/  if (session <= low) return;/"
runm rr_tick_no_session_check  $F "s/  if (session <= low || ovr == nullptr) return;/  if (ovr == nullptr) return;/"
runm rr_tick_lobby_flag        $F "s/level::Classify(game, g_lobby.load(std::memory_order_acquire), \&level)/level::Classify(game, false, \&level)/"
runm rr_tick_in_kind           $F "s/  const bool in = kind == level::Kind::kCombatOrLobby;/  const bool in = kind != level::Kind::kCombatOrLobby;/"
runm rr_tick_unchanged         $F "s/  if (in == g_inLevel.load(std::memory_order_acquire)) return;/  if (in != g_inLevel.load(std::memory_order_acquire)) return;/"
runm rr_tick_level_not_kept    $F "s/  g_inLevel.store(in, std::memory_order_release);/  (void)in;/"
runm rr_tick_rate_swapped      $F "s/  const float rate = in ? low : session;/  const float rate = in ? session : low;/"
runm rr_tick_ask_session       $F "s/  const int result = Ask(ovr, rate);/  const int result = Ask(ovr, session);/"
runm rr_tick_sent_kept         $F "s/  g_sent.store(rate, std::memory_order_relaxed);/  (void)rate;/"
runm rr_tick_uncounted         $F "s/g_switches.fetch_add(1, std::memory_order_relaxed) + 1;/g_switches.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm rr_tick_asked_value       $F "s/  const HzText asked = Hz(session);/  const HzText asked = Hz(rate);/"
runm rr_tick_level_text        $F "s/  std::snprintf(levelText, sizeof(levelText), \"%016llx\"/  std::snprintf(levelText, sizeof(levelText), \"%llx\"/"
runm rr_tick_level             $F "s/sentinel::LogLevel::kInfo, \"combat_refresh_rate_switch\"/sentinel::LogLevel::kDebug, \"combat_refresh_rate_switch\"/"
runm rr_tick_n                 $F "/\"combat_refresh_rate_switch\"/,/\"mode\"/s/{{\"n\", n},/{{\"n\", n + 1},/"
runm rr_tick_asked_text        $F "/\"combat_refresh_rate_switch\"/,/\"mode\"/s/{\"asked_hz\", asked.text}/{\"asked_hz\", asked.text[0] ? sent.text : asked.text}/"
runm rr_tick_sent_text         $F "/\"combat_refresh_rate_switch\"/,/\"mode\"/s/{\"sent_hz\", sent.text}/{\"sent_hz\", sent.text[0] ? asked.text : sent.text}/"
runm rr_tick_rc                $F "/\"combat_refresh_rate_switch\"/,/\"mode\"/s/{\"rc\", result}/{\"rc\", result * 0}/"
runm rr_tick_level_field       $F "s/{\"level\", levelText}/{\"level\", asked.text}/"
runm rr_tick_why_in            $F "s/{\"why\", in ? \"combat or lobby level loaded\"/{\"why\", !in ? \"combat or lobby level loaded\"/"
runm rr_tick_why_arena         $F "s/: kind == level::Kind::kEchoArena ?/: kind != level::Kind::kEchoArena ?/"
runm rr_tick_mode              $F "/\"combat_refresh_rate_switch\"/,/\"quest2\"/s/{\"mode\", ModeName()}/{\"mode\", \"match\"}/"
runm rr_tick_quest2            $F "/\"combat_refresh_rate_switch\"/,/\"quest2\"/s/g_quest2.load(std::memory_order_relaxed) ? 1 : 0/g_quest2.load(std::memory_order_relaxed) ? 0 : 1/"
runm rr_mode_witness           $F "s/  if (!g_on.load(std::memory_order_relaxed)) return \"witness\";/  (void)g_on;/"
runm rr_mode_swapped           $F "s/return g_lobby.load(std::memory_order_relaxed) ? \"lobby\" : \"match\";/return g_lobby.load(std::memory_order_relaxed) ? \"match\" : \"lobby\";/"
runm rr_published_hz           $F "s/  rule.levelHz = g_levelHz.load(std::memory_order_acquire);/  rule.levelHz = kLevelHz;/"
# the re-ask: marked by its thread, through the word now in the slot, with the game's ovr and the rate
runm rr_ask_not_marked         $F "s/  g_reaskThread.store(ThisThread(), std::memory_order_release);/  (void)fn;/"
runm rr_ask_not_cleared        $F "s/  g_reaskThread.store(0, std::memory_order_release);\$/  (void)result;/"
runm rr_ask_ovr                $F "s/  const int result = fn(ovr, hz);/  (void)ovr;\n  const int result = fn(nullptr, hz);/"
runm rr_ask_rate               $F "s/  const int result = fn(ovr, hz);/  const int result = fn(ovr, hz + 1.0f);/"
runm rr_ask_no_default         $F "s/  if (fn == nullptr) fn = \&AskThroughTheSlot;/  if (fn == nullptr \&\& false) fn = \&AskThroughTheSlot;/"
runm rr_slot_lookup_default    $F "s/  if (lookup == nullptr) lookup = \&sentinel::FindLoadedImage;/  (void)0;/"
runm rr_slot_no_image          $F "s/  if (!lookup(sentinel::pinned::kLibR15, \&image)) return -1;/  if (!lookup(sentinel::pinned::kLibR15, \&image)) return 0;/"
runm rr_slot_empty_word        $F "s/  if (word == nullptr) return -1;/  if (word == nullptr) return 0;/"
runm rr_slot_offset            $F "s/image.base + \*target.slotVaddr);/image.base + *target.slotVaddr + 8);/"
runm rr_slot_hz                $F "s/OvrMobileOpaque\*>(ovr), hz);/OvrMobileOpaque*>(ovr), hz + 1.0f);/"
# the headset: named by the profile's rules
runm rr_headset_override       $F "s/  if (override >= 0) return override != 0;/  if (override >= 0) return override == 0;/"
runm rr_headset_profile        $F "s/IdentifyDevice(device != nullptr ? device : \"\").profile->id == headset_profile::Id::kQuest2;/IdentifyDevice(device != nullptr ? device : \"\").profile->id != headset_profile::Id::kQuest2;/"
runm rr_headset_null           $F "s/IdentifyDevice(device != nullptr ? device : \"\")/IdentifyDevice(device)/"
# the table row (combat_install.cpp, shared) and its tests: each key reaches the module, the step is a normal one, the name is the module's
runm rrrow_rule_key        $CI "s/  return refresh_rate::Install(g_config.refreshRate.on, g_config.refreshRateLobby.on, g_config.refreshRateQuest2At72.on);/  return refresh_rate::Install(true, g_config.refreshRateLobby.on, g_config.refreshRateQuest2At72.on);/" quest-host-combat combat_install_test
runm rrrow_lobby_key       $CI "s/  return refresh_rate::Install(g_config.refreshRate.on, g_config.refreshRateLobby.on, g_config.refreshRateQuest2At72.on);/  return refresh_rate::Install(g_config.refreshRate.on, true, g_config.refreshRateQuest2At72.on);/" quest-host-combat combat_install_test
runm rrrow_q2_key          $CI "s/  return refresh_rate::Install(g_config.refreshRate.on, g_config.refreshRateLobby.on, g_config.refreshRateQuest2At72.on);/  return refresh_rate::Install(g_config.refreshRate.on, g_config.refreshRateLobby.on, true);/" quest-host-combat combat_install_test
runm rrrow_keys_rotated    $CI "s/  return refresh_rate::Install(g_config.refreshRate.on, g_config.refreshRateLobby.on, g_config.refreshRateQuest2At72.on);/  return refresh_rate::Install(g_config.refreshRateLobby.on, g_config.refreshRate.on, g_config.refreshRateQuest2At72.on);/" quest-host-combat combat_install_test
runm rrrow_early           $CI "s/Module{\"refresh_rate\", nullptr, \&RefreshRateNormal}/Module{\"refresh_rate\", \&RefreshRateNormal, nullptr}/" quest-host-combat combat_install_test
runm rrrow_name            $CI "s/Module{\"refresh_rate\", nullptr, \&RefreshRateNormal}/Module{\"refresh_rates\", nullptr, \&RefreshRateNormal}/" quest-host-combat combat_install_test
