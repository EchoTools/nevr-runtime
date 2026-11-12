# combat_scope: each rule of src/quest/combat/combat_scope.{h,cpp} removed or bent once; src/quest/tests/combat_scope_test.cpp must fail on every one,
# and so must the install table's and the tick's tests for the lines that touch them. Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
C=src/quest/combat/combat_scope.cpp
H=src/quest/combat/combat_scope.h
T=src/quest/combat/combat_tick.cpp
I=src/quest/combat/combat_install.cpp
# the look: spaced, from the lobby-inclusive scan, bits as the scan reports them
runm sc_look_every_call     $C "s/^  if (now < g_nextLookMs.load(std::memory_order_relaxed)) return;\$//"
runm sc_spacing             $H "s/kLookEveryMs = 250/kLookEveryMs = 2500/"
runm sc_map_bit_dropped     $C "s/(loaded.combatMap ? kBitCombatMap : 0U)/0U/"
runm sc_lobby_bit_dropped   $C "s/(loaded.combatLobby ? kBitCombatLobby : 0U)/0U/"
runm sc_arena_bit_dropped   $C "s/(loaded.echoArena ? kBitEchoArena : 0U)/0U/"
# the two answers, derived in one place
runm sc_arena_not_winning   $H "s/return (bits \\& kBitEchoArena) == 0 \\&\\& (bits \\& (kBitCombatMap | kBitCombatLobby)) != 0;/return (bits \\& (kBitCombatMap | kBitCombatLobby)) != 0;/"
runm sc_map_arena_not_winning $H "s/(kBitCombatMap | kBitEchoArena)) == kBitCombatMap;/kBitCombatMap) == kBitCombatMap;/"
runm sc_map_includes_lobby  $H "s/return (detail::g_bits.load(std::memory_order_relaxed) \\& (kBitCombatMap | kBitEchoArena)) == kBitCombatMap;/return (detail::g_bits.load(std::memory_order_relaxed) \\& (kBitCombatMap | kBitCombatLobby | kBitEchoArena)) != 0;/"
runm sc_lobby_not_counted   $H "s/(bits \\& (kBitCombatMap | kBitCombatLobby)) != 0;/(bits \\& kBitCombatMap) != 0;/"
runm sc_starts_open         $H "s/g_bits{0}/g_bits{kBitCombatMap}/"
# staleness: a true answer older than kStaleAfterMs is false
runm sc_never_stale         $C "s/relaxed) <= kStaleAfterMs;/relaxed) <= kStaleAfterMs || true;/"
runm sc_stale_by_one        $C "s/relaxed) <= kStaleAfterMs;/relaxed) < kStaleAfterMs;/"
runm sc_stale_limit         $H "s/kStaleAfterMs = 1000/kStaleAfterMs = 100000/"
runm sc_look_time_unstamped $C "s/^  detail::g_lookedAtMs.store(now, std::memory_order_relaxed);.*\$//"
runm sc_answers_skip_fresh  $H "s/return SeenCombatOrLobby() \\&\\& LookIsFresh();/return SeenCombatOrLobby();/"
runm sc_map_skips_fresh     $H "s/return SeenCombatMap() \\&\\& LookIsFresh();/return SeenCombatMap();/"
# the change line: one per change of either answer, with its values
runm sc_no_change_gate      $C "s/^  if (CombatOrLobby(bits) == CombatOrLobby(before) \\&\\& CombatMap(bits) == CombatMap(before)) return;\$//"
runm sc_change_ignores_map  $C "s/ \\&\\& CombatMap(bits) == CombatMap(before)) return;/) return;/"
runm sc_change_swapped      $C "s/{\"from\", CombatOrLobby(before) ? 1 : 0}, {\"to\", in ? 1 : 0}/{\"from\", in ? 1 : 0}, {\"to\", CombatOrLobby(before) ? 1 : 0}/"
runm sc_change_level_zero    $C "s/{\"level\", sentinel::HexString(levelText, levelId)}/{\"level\", sentinel::HexString(levelText, levelId \\& 0)}/"
runm sc_change_level_arena_last $C "s/loaded.echoArena ? loaded.echoArenaLevel : (loaded.combatMap ? loaded.combatMapLevel : loaded.combatLobbyLevel)/loaded.combatMap ? loaded.combatMapLevel : (loaded.combatLobby ? loaded.combatLobbyLevel : loaded.echoArenaLevel)/"
# the row: registers once, only with the tick installed, and says so
runm sc_row_twice           $C "s/^  if (g_registered.load(std::memory_order_acquire)) return {true, \"installed\"};\$//"
runm sc_row_ignores_tick    $C "s/const bool added = tickUp \\&\\& tick::Add(\\&Refresh);/const bool added = tick::Add(\\&Refresh);/"
runm sc_row_refusal_kept    $C "s/^  if (added) g_registered.store(true, std::memory_order_release);\$/  g_registered.store(true, std::memory_order_release);/"
runm sc_row_refusal_quiet   $C "s/added ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_scope_install\"/sentinel::LogLevel::kInfo, \"combat_scope_install\"/"
runm sc_live_without_tick   $C "s/return g_registered.load(std::memory_order_acquire) \\&\\& tick::Installed();/return g_registered.load(std::memory_order_acquire);/"
runm sc_live_without_row    $C "s/return g_registered.load(std::memory_order_acquire) \\&\\& tick::Installed();/return tick::Installed();/"
# the tick says whether it is installed
runm tick_installed_never   $T "s/^  g_installed.store(ok, std::memory_order_release);\$//" quest-host-combat combat_scope_test
runm tick_installed_always  $T "s/^  g_installed.store(ok, std::memory_order_release);\$/  g_installed.store(true, std::memory_order_release);/" quest-host-combat combat_scope_test
# the install table: the row sits after the tick and before every reader
runm table_scope_registers_nothing $I "s/^  return scope::InstallNormal();\$/  return {true, \"installed\"};/" quest-host-combat combat_install_test
# the real clock, in milliseconds (the fake clock of the other tests cannot see its conversion)
runm sc_clock_no_seconds_factor $C "s/static_cast<std::int64_t>(ts.tv_sec) \\* 1000 + /static_cast<std::int64_t>(ts.tv_sec) + /"
runm sc_clock_seconds_divided   $C "s/static_cast<std::int64_t>(ts.tv_sec) \\* 1000 + /static_cast<std::int64_t>(ts.tv_sec) \\/ 1000 + /"
runm sc_clock_nsec_per_us       $C "s/ts.tv_nsec \\/ 1000000;/ts.tv_nsec \\/ 1000;/"
runm sc_clock_nsec_dropped      $C "s/ + ts.tv_nsec \\/ 1000000;/;/"
# the stale record: once per gap, when the tick is back, only after a combat answer, past the limit, with the gap and the level held
runm sc_stale_never_said        $C "s/if (CombatOrLobby(before) \\&\\& now - last > kStaleAfterMs) {/if (false) {/"
runm sc_stale_at_limit          $C "s/CombatOrLobby(before) \\&\\& now - last > kStaleAfterMs) {/CombatOrLobby(before) \\&\\& now - last >= kStaleAfterMs) {/"
runm sc_stale_after_arena       $C "s/if (CombatOrLobby(before) \\&\\& now - last > kStaleAfterMs) {/if (now - last > kStaleAfterMs) {/"
runm sc_stale_quiet             $C "s/sentinel::LogLevel::kWarn, \"combat_scope_stale\"/sentinel::LogLevel::kInfo, \"combat_scope_stale\"/"
runm sc_stale_gap_zero          $C "s/{\"gap_ms\", static_cast<std::int64_t>(now - last)}/{\"gap_ms\", 0}/"
runm sc_stale_limit_zero        $C "s/{\"stale_after_ms\", kStaleAfterMs}/{\"stale_after_ms\", 0}/"
runm sc_stale_level_zero        $C "s/HexString(levelText, g_lastLevel.load(std::memory_order_relaxed))/HexString(levelText, 0)/"
runm sc_stale_level_unstored    $C "s/^  g_lastLevel.store(levelId, std::memory_order_relaxed);\$//"
runm sc_stale_every_look        $C "s/^  detail::g_lookedAtMs.store(now, std::memory_order_relaxed);\$/  if (now - last <= kStaleAfterMs) detail::g_lookedAtMs.store(now, std::memory_order_relaxed);/"
# the row is on only while a reader of the scope is on
runm sc_row_ignores_readers     $I "s/  if (!anyReader) return/  if (false \\&\\& !anyReader) return/" quest-host-combat combat_install_test
runm sc_row_off_is_failed       $I "s/return {false, \"off_config\", \\/\\*off=\\*\\/true};/return {false, \"off_config\", \\/\\*off=\\*\\/false};/" quest-host-combat combat_install_test
runm sc_row_hudmeter_not_reader $I "s/const bool anyReader = g_config.hudmeter.on || /const bool anyReader = /" quest-host-combat combat_install_test
runm sc_row_gamma_not_reader    $I "s/ || g_config.shieldgammaGamma.on || / || /" quest-host-combat combat_install_test
runm sc_row_mirror_not_reader   $I "s/ || g_config.shieldgammaTubeMirror.on || / || /" quest-host-combat combat_install_test
# the look record (LastLook): one look whole, under the sequence count, on any thread; and the arena question built on it (SeenEchoArena)
runm lr_odd_ignored             $C "s/^    if ((begin \\& 1U) != 0) continue;  \\/\\/ a look is being written\$//"
runm lr_end_unchecked           $C "s/^    if (g_seq.load(std::memory_order_relaxed) != begin) continue;.*\$//"
runm lr_write_not_odd           $C "s/^  g_seq.store(seq + 1, std::memory_order_relaxed);\$//"
runm lr_write_left_odd          $C "s/^  g_seq.store(seq + 2, std::memory_order_release);\$/  (void)seq;/"
runm lr_no_tries                $H "s/kLookReadTries = 4;/kLookReadTries = 0;/"
runm lr_gives_up_open           $C "s/^  return Look{};  \\/\\/ never whole.*\$/  Look open; open.fresh = true; return open;/"
runm lr_fresh_without_look      $C "s/look.fresh = look.looks != 0 \\&\\& NowMs()/look.fresh = NowMs()/"
runm lr_fresh_by_one            $C "s/look.fresh = look.looks != 0 \\&\\& NowMs() - at <= kStaleAfterMs;/look.fresh = look.looks != 0 \\&\\& NowMs() - at < kStaleAfterMs;/"
runm lr_never_stale             $C "s/look.fresh = look.looks != 0 \\&\\& NowMs() - at <= kStaleAfterMs;/look.fresh = look.looks != 0 \\&\\& (NowMs() - at <= kStaleAfterMs || true);/"
runm lr_bits_zero               $C "s/look.bits = detail::g_bits.load(std::memory_order_relaxed);/look.bits = 0;/"
runm lr_level_zero              $C "s/look.level = g_lastLevel.load(std::memory_order_relaxed);/look.level = 0;/"
runm lr_looks_zero              $C "s/look.looks = g_looks.load(std::memory_order_relaxed);/look.looks = 1;/"
runm lr_seam_not_called         $C "s/^  if (seam != nullptr) seam();\$/  (void)seam;/"
runm sea_ignores_stale          $C "s/return !look.fresh || (look.bits \\& kBitEchoArena) != 0;/return (look.bits \\& kBitEchoArena) != 0;/"
runm sea_ignores_arena          $C "s/return !look.fresh || (look.bits \\& kBitEchoArena) != 0;/return !look.fresh;/"
runm sea_inverted               $C "s/return !look.fresh || (look.bits \\& kBitEchoArena) != 0;/return look.fresh \\&\\& (look.bits \\& kBitEchoArena) == 0;/"
# the texts of the three lines name every reader of the answer, not two of them
runm sc_change_msg_old          $C "s/a combat level is loaded: the combat modules that read the level answer act/a combat level is loaded: the wrist HUD and shield colour modules act/"
runm sc_change_out_msg_old      $C "s/no combat level is loaded: the combat modules that read the level answer pass every call through/no combat level is loaded: the wrist HUD and shield colour modules pass every call through/"
runm sc_stale_msg_old           $C "s/every combat module that reads it passed every call /the wrist HUD and shield colour modules passed every call /"
runm sc_install_msg_old         $C "s/, four times a second, for every combat module that reads it\"/, four times a second\"/"
runm sc_refused_msg_old         $C "s/so the combat modules that read it are not installed/so the modules that read it are not installed/"
# every field of a look is written inside its sequence count (a field stored after the close is a torn read on another thread: the closed-count seam,
# which CloseLook calls right after the close, sees it; each mutant moves one store to after the CloseLook call, the review's H2 for the level)
runm lr_level_after_close       $C "/^  g_lastLevel.store(levelId, std::memory_order_relaxed);\$/d; s/^  CloseLook(seq);\$/&\n  g_lastLevel.store(levelId, std::memory_order_relaxed);/"
runm lr_bits_after_close        $C "/^  detail::g_bits.store(bits, std::memory_order_release);\$/d; s/^  CloseLook(seq);\$/&\n  detail::g_bits.store(bits, std::memory_order_release);/"
runm lr_stamp_after_close       $C "/^  detail::g_lookedAtMs.store(now, std::memory_order_relaxed);\$/d; s/^  CloseLook(seq);\$/&\n  detail::g_lookedAtMs.store(now, std::memory_order_relaxed);/"
runm lr_closed_seam_not_called  $C "s/^  if (closed != nullptr) closed();\$/  (void)closed;/"
runm sea_record_ignores_stale   $C "s/^bool SeenEchoArena(const Look\\& look) noexcept { return !look.fresh || /bool SeenEchoArena(const Look\\& look) noexcept { return /"
# InCombatMap on a record (dronekill's change line): the freshness, the arena and the lobby rules, each dropped once
runm icm_record_ignores_stale   $H 's/^inline bool InCombatMap(const Look\& look) noexcept { return look.fresh \&\& /inline bool InCombatMap(const Look\& look) noexcept { return /'
runm icm_record_ignores_arena   $H 's/^\(inline bool InCombatMap(const Look\& look) noexcept { return look.fresh \&\& \)(look.bits \& (kBitCombatMap | kBitEchoArena)) == kBitCombatMap; }/\1(look.bits \& kBitCombatMap) != 0; }/'
runm icm_record_lobby           $H 's/^\(inline bool InCombatMap(const Look\& look) noexcept { return look.fresh \&\& \)(look.bits \& (kBitCombatMap | kBitEchoArena)) == kBitCombatMap; }/\1(look.bits \& kBitEchoArena) == 0 \&\& (look.bits \& (kBitCombatMap | kBitCombatLobby)) != 0; }/'
# InCombatOrLobby on a record and the class of a look (ec's level_class, powerboost's kind)
runm icl_record_ignores_stale   $H 's/^  return look.fresh \&\& (look.bits \& kBitEchoArena) == 0 \&\& (look.bits \& (kBitCombatMap | kBitCombatLobby)) != 0;$/  return (look.bits \& kBitEchoArena) == 0 \&\& (look.bits \& (kBitCombatMap | kBitCombatLobby)) != 0;/'
runm icl_record_ignores_arena   $H 's/^  return look.fresh \&\& (look.bits \& kBitEchoArena) == 0 \&\& (look.bits \& (kBitCombatMap | kBitCombatLobby)) != 0;$/  return look.fresh \&\& (look.bits \& (kBitCombatMap | kBitCombatLobby)) != 0;/'
runm icl_record_no_lobby        $H 's/^  return look.fresh \&\& (look.bits \& kBitEchoArena) == 0 \&\& (look.bits \& (kBitCombatMap | kBitCombatLobby)) != 0;$/  return look.fresh \&\& (look.bits \& kBitEchoArena) == 0 \&\& (look.bits \& kBitCombatMap) != 0;/'
runm cls_stale_is_no_look       $C 's/return look.looks == 0 ? LookClass::kNoLook : LookClass::kStale;/return LookClass::kNoLook;/'
runm cls_fresh_unchecked        $C 's/^  if (!look.fresh) return look.looks == 0/  if (look.looks == 0) return look.looks == 0/'
runm cls_arena_ignored          $C '/^  if ((look.bits \& kBitEchoArena) != 0) return LookClass::kEchoArena;$/d'
runm cls_lobby_is_other         $C 's/^  if (CombatOrLobby(look.bits)) return LookClass::kCombatOrLobby;$/  if (CombatMap(look.bits)) return LookClass::kCombatOrLobby;/'
runm cls_name_stale             $C 's/case LookClass::kStale: return "stale";/case LookClass::kStale: return "no_look";/'
runm cls_name_other             $C 's/case LookClass::kOther: return "other";/case LookClass::kOther: return "none";/'
