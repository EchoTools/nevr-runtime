# music: each rule of src/quest/combat/music.cpp, music.h and music_targets.h removed or bent once; src/quest/tests/music_test.cpp must fail on every one.
# Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target>]`.
S=src/quest/combat/music.cpp
H=src/quest/combat/music.h
T=src/quest/combat/music_targets.h
CI=src/quest/combat/combat_install.cpp
# the bank: every id, and the table's size
runm mu_play0_id        $H "s/kPlay\[2\] = {0x2f22c384/kPlay[2] = {0x2f22c385/"
runm mu_play1_id        $H "s/0xb1b06cfb};/0xb1b06cfa};/"
runm mu_stop0_id        $H "s/kStop\[2\] = {0xde586538/kStop[2] = {0xde586539/"
runm mu_stop1_id        $H "s/0x2acba759};/0x2acba75a};/"
runm mu_state_first     $H "s/0x045d3c23/0x045d3c24/"
runm mu_state_mid       $H "s/0xd9a48db1/0xd9a48db2/"
runm mu_state_last      $H "s/0xffae8834};/0xffae8835};/"
runm mu_search_skips_one   $S "s/      lo = mid + 1;/      lo = mid + 2;/"
runm mu_search_wrong_kind $S "s/    if (kBank\[mid\].id == event) return kBank\[mid\].kind;/    if (kBank[mid].id == event) return Kind::kState;/"
# the pinned slot, function and words
runm mu_slot_const      $T "s/kSlotVaddr = 0x36d76e8ULL/kSlotVaddr = 0x36d76e0ULL/"
runm mu_stock_const     $T "s/kStockVaddr = 0x2549264ULL/kStockVaddr = 0x2549260ULL/"
runm mu_pin_first_word  $T "s/{0x2549264, 0xd10183ff}/{0x2549264, 0xd10183fe}/"
runm mu_pin_flags_word  $T "s/{0x254927c, 0xb9400068}/{0x254927c, 0xb9400069}/"
runm mu_pin_last_addr   $T "s/{0x25492d4, 0x34000b73}/{0x25492d8, 0x34000b73}/"
runm mu_target_wrong_expected $T "s/base + pins.stockVaddr);/base + pins.stockVaddr + 4);/"
runm mu_pins_ignored    $S "s/  if (FirstPinMismatch(reinterpret_cast<const unsigned char\*>(image.base)) != nullptr) return refuse(\"pin_mismatch\", \"none\");/  (void)FirstPinMismatch;/"
runm mu_pin_loop_early  $S "s/    if (found != pin.word) return \"post_sound_event_code\";/    if (found != pin.word \&\& \&pin != \&tgt::kWords[tgt::kWordCount - 1]) return \"post_sound_event_code\";/"
# the install: the switch, the tick, the build
runm mu_off_installs    $S "s/  if (!configOn) {/  if (!configOn \&\& pins.slotVaddr == 0) {/"
runm mu_off_reason      $S "s/return {false, \"off_config\"};/return {false, \"off\"};/"
runm mu_no_tick         $S "s/    if (!tick::Add(\&Tick)) return refuse(\"tick_table_full\", \"none\");/    g_tickRegistered.store(true);/"
runm mu_no_build_check  $S "s/  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return refuse(\"build_id_mismatch\", \"none\");//"
runm mu_legacy_not_said $S "s/  if (!legacy::Present(\"music.off\")) return;/  return;/"
# the scope: where a Play acts
runm mu_play_unscoped   $S "s/    inCombatMap = InCombatMapNow();/    inCombatMap = InCombatMapNow() || true;/"
runm mu_scope_lobby     $S "s/  return level::Classify(game, \/\*lobby=\*\/false, \&levelId, \/\*inFlight=\*\/true) == level::Kind::kCombatOrLobby;/  return level::Classify(game, \/*lobby=*\/true, \&levelId, \/*inFlight=*\/true) == level::Kind::kCombatOrLobby;/"
runm mu_scope_arena_wins $S "s/  return level::Classify(game, \/\*lobby=\*\/false, \&levelId, \/\*inFlight=\*\/true) == level::Kind::kCombatOrLobby;/  return level::Classify(game, \/*lobby=*\/false, \&levelId, \/*inFlight=*\/true) != level::Kind::kNone;/"
runm mu_unknown_game_acts $S "s/  if (game == 0) return false;  \/\/ no frame has run yet: the level is unknown, so the Play goes through untouched/  if (game == 0) return true;/"
runm mu_game_not_published $S "s/  if (g_game.load(std::memory_order_relaxed) != game) g_game.store(game, std::memory_order_release);//"
# the Stops before the Play
runm mu_one_stop_counted $S "s/  for (const std::uint32_t stop : kStop) PostStop(original, self, stop, object);\n  return 2;/&/;s/^  return 2;\$/  return 1;/"
runm mu_stop_flags      $S "s/  std::uint32_t flags = 0;/  std::uint32_t flags = 1;/"
runm mu_stop_seek       $S "s/  original(self, stop, object, \&flags, nullptr, nullptr, 0, \&playingId);/  original(self, stop, object, \&flags, nullptr, nullptr, 1, \&playingId);/"
runm mu_stop_callback   $S "s/  original(self, stop, object, \&flags, nullptr, nullptr, 0, \&playingId);/  original(self, stop, object, \&flags, \&flags, nullptr, 0, \&playingId);/"
runm mu_stop_no_id      $S "s/  original(self, stop, object, \&flags, nullptr, nullptr, 0, \&playingId);/  original(self, stop, object, \&flags, nullptr, nullptr, 0, nullptr);/;s/^  std::uint32_t playingId = 0;//"
runm mu_prev_same_object $S "s/      if (previous != object \&\& previous != kNoObject) stops += PostBothStops(original, self, previous);/      if (previous != kNoObject) stops += PostBothStops(original, self, previous);/"
runm mu_prev_sentinel   $S "s/      if (previous != object \&\& previous != kNoObject) stops += PostBothStops(original, self, previous);/      if (previous != object) stops += PostBothStops(original, self, previous);/"
runm mu_prev_never      $S "s/      previous = g_lastObject.exchange(object, std::memory_order_acq_rel);/      previous = g_lastObject.load(std::memory_order_acquire); g_lastObject.store(kNoObject);/"
runm mu_result_dropped  $S "s/^  return result;\$/  return result \& 0xff;/"
runm mu_non_bank_counted $S "s/  if (kind == Kind::kNone) return original(self, event, object, flags, callback, cookie, seekMs, playingId);.*/  if (kind == Kind::kNone) { g_bankPosts.fetch_add(1); return original(self, event, object, flags, callback, cookie, seekMs, playingId); }/"
runm mu_flags_dropped   $S "s/  const std::uint64_t result = original(self, event, object, flags, callback, cookie, seekMs, playingId);/  const std::uint64_t result = original(self, event, object, nullptr, callback, cookie, seekMs, playingId);/"
runm mu_seek_dropped    $S "s/  const std::uint64_t result = original(self, event, object, flags, callback, cookie, seekMs, playingId);/  const std::uint64_t result = original(self, event, object, flags, callback, cookie, 0, playingId);/"
# the unload tick
runm mu_tick_period_zero $S "s/  g_nextLookMs.store(now + static_cast<std::int64_t>(kTickPeriodMs), std::memory_order_relaxed);/  g_nextLookMs.store(now, std::memory_order_relaxed);/"
runm mu_tick_inverted   $S "s/  if (level::CombatMapLoaded(game, \&levelId)) return;/  if (!level::CombatMapLoaded(game, \&levelId)) return;/"
runm mu_tick_repeats    $S "s/  if (!g_playing.exchange(false, std::memory_order_acq_rel)) return;/  if (!g_playing.load(std::memory_order_acquire)) return;/"
runm mu_tick_wrong_object $S "s/  const std::uint32_t object = g_lastObject.load(std::memory_order_acquire);/  const std::uint32_t object = 0;/"
runm mu_tick_wrong_self $S "s/  void\* const self = reinterpret_cast<void\*>(g_self.load(std::memory_order_relaxed));/  void* const self = nullptr;/"
runm mu_play_not_armed  $S "s/      g_playing.store(true, std::memory_order_release);/      (void)g_playing;/"
runm mu_tick_before_play $S "s/  if (!g_playing.load(std::memory_order_acquire)) return;  \/\/ arena, and a combat session before its first Play: one load/  g_playing.store(true);/"
runm mu_tick_counts_not $S "s/  g_tickStops.fetch_add(1, std::memory_order_relaxed);//"
# the lines
runm mu_first_every     $S "s/  if (g_firstSaid.exchange(true, std::memory_order_acq_rel)) return;//"
runm mu_play_line_cap   $S "s/g_playLines.fetch_add(1, std::memory_order_relaxed) < kMaxPlayLines/g_playLines.fetch_add(1, std::memory_order_relaxed) < kMaxPlayLines + 1/"
runm mu_out_of_scope_uncounted $S "s/      g_playsOutOfScope.fetch_add(1, std::memory_order_relaxed);//"
runm mu_install_line_level $S "s/  LogInstall(sentinel::LogLevel::kInfo, \"on\", \"default\"/  LogInstall(sentinel::LogLevel::kWarn, \"on\", \"default\"/"
runm mu_refusal_level   $S "s/    LogInstall(sentinel::LogLevel::kError, \"refused\", reason, status, kRefusedMsg);/    LogInstall(sentinel::LogLevel::kInfo, \"refused\", reason, status, kRefusedMsg);/"
# the switch's key and row (shared files)
runm mu_ci_row_ignores_key $CI "s/music::Install(g_config.music.on)/music::Install(true)/" quest-host-combat combat_install_test
runm mu_qc_default_off  src/quest/sentinel/quest_config.cpp "s/  r.config.combat.music = ReadCombatSwitch(cfg, kCombatMusicKey, r);/  r.config.combat.music = ReadCombatSwitch(cfg, kCombatMusicKey, r, false);/" quest-host-shared quest_config_test
runm mu_qc_not_read     src/quest/sentinel/quest_config.cpp "s/  r.config.combat.music = ReadCombatSwitch(cfg, kCombatMusicKey, r);/  r.config.combat.music = CombatSwitch{};/" quest-host-shared quest_config_test

runm mu_ci_row_key       $CI "s/\"combat.music\", \&nevr_quest::CombatConfig::music}/\"combat.musik\", \&nevr_quest::CombatConfig::music}/" quest-host-combat combat_install_test
runm mu_ci_row_member    $CI "s/\"combat.music\", \&nevr_quest::CombatConfig::music}/\"combat.music\", \&nevr_quest::CombatConfig::sensorwd}/" quest-host-combat combat_install_test
# arena pays nothing, by count (#2277 review): a level scan on every post outside the bank, a scan on every bank post, and the map-to-map ask without the init-fiber table
runm mu_scan_on_every_post $S "s/^  if (kind == Kind::kNone) return original(/  (void)InCombatMapNow();\n&/"
runm mu_scan_on_every_bank_post $S "s/^  g_bankPosts.fetch_add(1, std::memory_order_relaxed);/&\n  (void)InCombatMapNow();/"
runm mu_ask_not_in_flight $S "s/\/\*inFlight=\*\/true/\/*inFlight=*\/false/"
