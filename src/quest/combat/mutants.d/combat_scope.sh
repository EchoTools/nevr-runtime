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
runm sc_never_stale         $C "s/<= kStaleAfterMs; }/<= kStaleAfterMs || true; }/"
runm sc_stale_by_one        $C "s/<= kStaleAfterMs; }/< kStaleAfterMs; }/"
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
runm table_scope_row_gone   $I "s/^    Module{\"scope\", nullptr, \\&scope::InstallNormal},\$//" quest-host-combat combat_install_test
