# peerclamp: each rule of src/quest/combat/peerclamp.cpp, peerclamp.h and peerclamp_targets.h removed or bent once; src/quest/tests/peerclamp_test.cpp
# must fail on every one (the shared files it changed are each run against the test that covers them). Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
P=src/quest/combat/peerclamp.cpp
T=src/quest/combat/peerclamp_targets.h
H=src/quest/combat/peerclamp.h
# the decision: a key that is not kInvalid, a stamp above now
runm pc_stamp_ge          $P "s/ != 0 \&\& stamp > now) {/ != 0 \&\& stamp >= now) {/"
runm pc_stamp_ne          $P "s/ != 0 \&\& stamp > now) {/ != 0 \&\& stamp != now) {/"
runm pc_no_stamp_test     $P "s/ != 0 \&\& stamp > now) {/ != 0 \&\& stamp + 1 > 0) {/"
runm pc_no_key_test       $P "s/if (std::memcmp(entry + (kOffKey - kOffNext), invalid, kKeyLen) != 0 \&\& stamp > now) {/if (entry[0] == entry[0] \&\& stamp > now) {/"
# the layout the stock loop reads
# (kStride, kOffHead, kOffPerChunk, kOffChunks, kOffNow, kOffNext, kOffKey, kOffStamp and kKeyLen are tied to the pinned words by static_assert in peerclamp_targets.h: a changed one fails the build)
runm pc_sent_off          $T "s/kOffSent = 0x470;/kOffSent = 0x468;/"
runm pc_end_marker        $T "s/kEnd = 0xffffffffU;/kEnd = 0xfffffffeU;/"
# the one write
runm pc_no_write          $P "s/      \*reinterpret_cast<std::uint64_t\*>(e + kOffStamp) = now;/      (void)now;/"
runm pc_write_value       $P "s/      \*reinterpret_cast<std::uint64_t\*>(e + kOffStamp) = now;/      *reinterpret_cast<std::uint64_t*>(e + kOffStamp) = now - 1;/"
runm pc_write_now         $P "s/      \*reinterpret_cast<std::uint64_t\*>(e + kOffStamp) = now;/      *reinterpret_cast<std::uint64_t*>(e + kOffStamp) = now; *reinterpret_cast<std::uint64_t*>(bd + kOffNow) = stamp;/"
# the reads
runm pc_invalid_unread    $P "s/  if (!SafeRead(g_invalid.load(std::memory_order_relaxed), invalid, sizeof(invalid))) return;/  (void)SafeRead(g_invalid.load(std::memory_order_relaxed), invalid, sizeof(invalid));/"
runm pc_per_zero          $P "s/  if (per == 0) return;/  (void)per;/"
runm pc_chunk_index       $P "s/(idx \/ per) \* sizeof(std::uintptr_t)/(idx % per) * sizeof(std::uintptr_t)/"
runm pc_slot_index        $P "s/(idx % per) \* kStride/(idx \/ per) * kStride/"
runm pc_chunk_width       $P "s/ \* sizeof(std::uintptr_t), \&chunk, sizeof(chunk));/ * 4, \&chunk, sizeof(chunk));/"
runm pc_entry_unread      $P "s/    if (!SafeRead(e + kOffNext, entry, sizeof(entry))) return;/    (void)SafeRead(e + kOffNext, entry, sizeof(entry));/"
# the walk cap
runm pc_walk_cap_low      $H "s/kMaxWalk = 4096;/kMaxWalk = 4095;/"
runm pc_walk_cap_high     $H "s/kMaxWalk = 4096;/kMaxWalk = 4097;/"
runm pc_walk_no_cap       $P "s/    if (n == kMaxWalk) {/    if (n == kMaxWalk \&\& false) {/"
runm pc_cap_every         $P "s/      if (!g_capLogged.exchange(true, std::memory_order_relaxed)) {/      if (true) {/"
runm pc_cap_level         $P "s/sentinel::LogLevel::kWarn, \"combat_peerclamp_walk_capped\"/sentinel::LogLevel::kInfo, \"combat_peerclamp_walk_capped\"/"
runm pc_cap_not_counted   $P "s/      g_capped.fetch_add(1, std::memory_order_relaxed);//"
runm pc_cap_returns       $P "/    if (n == kMaxWalk) {/,/^    }\$/{s/^      return;\$/      (void)0;/}"
runm pc_walked_not_counted $P "s/    g_walked.fetch_add(1, std::memory_order_relaxed);//"
# the game's function: called once, with the same argument, after the clamp
runm pc_no_original       $P "s/^  original(bd);\$/  (void)original;/"
runm pc_original_arg      $P "s/^  original(bd);\$/  original(nullptr);/"
runm pc_original_first     $P "/^  Clamp(reinterpret_cast<std::uintptr_t>(bd));/{N;N;s/\(.*\)\n\(.*\)\n\(.*\)/\3\n\1\n\2/}"
runm pc_out_of_scope_no_original $P "s/^    original(bd);\$/    (void)original;/"
# the scope: a combat map or the combat lobby
runm pc_scope_ignored       $P "s/  if (!scope::SeenCombatOrLobby()) {/  if (false) {/"
runm pc_scope_inverted      $P "s/  if (!scope::SeenCombatOrLobby()) {/  if (scope::SeenCombatOrLobby()) {/"
runm pc_scope_map_only      $P "s/  if (!scope::SeenCombatOrLobby()) {/  if (!scope::SeenCombatMap()) {/"
runm pc_scope_not_counted $P "s/g_inScope.fetch_add(1, std::memory_order_relaxed) + 1;/g_inScope.fetch_add(0, std::memory_order_relaxed) + 1;/"
# the clamp lines
runm pc_lines             $H "s/kClampLines = 16;/kClampLines = 17;/"
runm pc_total_every       $H "s/kTotalEvery = 100;/kTotalEvery = 101;/"
runm pc_total_lines       $H "s/kTotalLines = 8;/kTotalLines = 9;/"
runm pc_no_total          $P "s/  } else if (n % kTotalEvery == 0 \&\&\$/  } else if (false \&\&/"
runm pc_not_counted       $P "s/g_clamped.fetch_add(1, std::memory_order_relaxed) + 1/g_clamped.fetch_add(0, std::memory_order_relaxed) + 1/"
runm pc_diff              $P "s/static_cast<unsigned long long>(stamp - now));/static_cast<unsigned long long>(now - stamp));/"
runm pc_line_idx          $P "s/{{\"n\", n}, {\"idx\", idx},/{{\"n\", n}, {\"idx\", idx + 1},/"
runm pc_line_sent         $P "s/{\"sent\", sentText},/{\"sent\", nowText},/"
runm pc_line_level        $P "s/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_peerclamp_clamped\",/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_peerclamp_clamped\",/"
# the stats
runm pc_stats_never       $P "s/  if ((inScope \& (kStatsEvery - 1)) == 0) LogStats(false);/  (void)inScope;/"
runm pc_stats_every       $P "s/  if ((inScope \& (kStatsEvery - 1)) == 0) LogStats(false);/  (void)inScope; LogStats(false);/"
runm pc_stats_every_wrong $H "s/kStatsEvery = 4096;/kStatsEvery = 2048;/"
runm pc_stats_final_never $P "s/  if (g_hook.installed()) LogStats(true);/  (void)g_hook;/"
runm pc_stats_final_always $P "s/  if (g_hook.installed()) LogStats(true);/  LogStats(true);/"
runm pc_stats_final_debug $P "s/final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, \"combat_peerclamp_stats\"/sentinel::LogLevel::kDebug, \"combat_peerclamp_stats\"/"
runm pc_stats_clamped     $P "s/{\"clamped\", g_clamped.load(std::memory_order_relaxed)},/{\"clamped\", 0},/"
# the install: the slot must hold CleanupPeers itself, the pins gate it, the build ID gates it, kInvalid must be readable, the tick is needed
runm pc_no_expected_word  $P "s/  target.expectedOriginal = reinterpret_cast<const void\*>(image.base + pins.fn);/  (void)pins.fn;/"
runm pc_expected_wrong    $P "s/image.base + pins.fn);/image.base + pins.fn + 4);/"
runm pc_pins_skipped        $P "s/  if (pinned::FirstFailure(image.base, \&kWordPins, 1, \&word)) return Refused(\"pinned_word_mismatch\", \&word);/  if (false) return Refused(\"pinned_word_mismatch\", \&word);/"
runm pc_last_pin_skipped    $T "s/kWordPins = {kWords, kWordCount, nullptr, 0};/kWordPins = {kWords, kWordCount - 1, nullptr, 0};/"
runm pc_pin_unreadable_ok $P "s/word->readable ? sentinel::HexString(haveText, word->have) : \"unreadable\"/sentinel::HexString(haveText, word->have)/"
runm pc_build_id_skipped    $P "s/  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\");//"
runm pc_no_build_id_ok    $P "s/  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\");//"
runm pc_invalid_null      $P "s/ || invalid == 0) return Refused(\"invalid_unreadable\");/) return Refused(\"invalid_unreadable\");/"
runm pc_invalid_unread_ok $P "s/  if (!SafeRead(image.base + static_cast<std::uintptr_t>(pins.invalid), \&invalid, sizeof(invalid)) || invalid == 0)/  if (false || invalid == 0)/"
runm pc_invalid_not_kept  $P "s/  g_invalid.store(invalid, std::memory_order_relaxed);//"
runm pc_refusal_quiet     $P "s/sentinel::LogFields(sentinel::LogLevel::kError, \"combat_peerclamp_install\",/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_peerclamp_install\",/"
runm pc_install_level     $P "s/sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_peerclamp_install\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_peerclamp_install\"/"
# the switch
runm pc_off_ignored       $P "s/  if (!on) {/  if (!on \&\& false) {/"
runm pc_off_not_off       $P "s/    return {false, \"off_config\", true};/    return {false, \"off_config\", false};/"
# the pinned constants
runm pc_slot_const        $T "s/kCleanupSlotVaddr = 0x36efad8ULL;/kCleanupSlotVaddr = 0x36efad0ULL;/"
runm pc_real_const        $T "s/kCleanupVaddr = 0x24ef9b0ULL;/kCleanupVaddr = 0x24ef9b8ULL;/"
runm pc_invalid_const     $T "s/kInvalidWordVaddr = 0x371d538ULL;/kInvalidWordVaddr = 0x371d530ULL;/"
runm pc_table_word        $T "s/0x24ef9b0, 0x6db733ed}/0x24ef9b0, 0x6db733ec}/"
runm pc_table_last        $T "s/0xf307ec, 0xd61f0220}/0xf307f0, 0xd61f0220}/"
# the table row (combat_install.cpp): the switch is read, in the normal phase, with an end step
CI=src/quest/combat/combat_install.cpp
runm pc_row_ignores_switch $CI "s/peerclamp::Install(g_config.peerclamp.on)/peerclamp::Install(true)/" quest-host-combat combat_install_test
runm pc_row_end_null      $CI "s/peerclamp, \&peerclamp::NoteEnd}/peerclamp, nullptr}/" quest-host-combat combat_install_test
runm pc_row_early         $CI "s/Module{\"peerclamp\", nullptr, \&PeerclampNormal,/Module{\"peerclamp\", \&PeerclampNormal, nullptr,/" quest-host-combat combat_install_test
# the key
QC=src/quest/sentinel/quest_config.cpp
runm pc_qc_default_off    $QC "s/  r.config.combat.peerclamp = ReadCombatSwitch(cfg, kCombatPeerclampKey, r);/  r.config.combat.peerclamp = ReadCombatSwitch(cfg, kCombatPeerclampKey, r, false);/" quest-host-shared quest_config_test
runm pc_qc_key_typo       $QC "s/kCombatPeerclampKey = \"combat.peerclamp\";/kCombatPeerclampKey = \"combat.peerclamps\";/" quest-host-shared quest_config_test
runm pc_qc_not_assigned   $QC "s/  r.config.combat.peerclamp = ReadCombatSwitch(cfg, kCombatPeerclampKey, r);/  (void)ReadCombatSwitch(cfg, kCombatPeerclampKey, r);/" quest-host-shared quest_config_test
runm pc_nc_known_key      src/core/nevr_config.cpp "s/\"droneguard\", \"droneguard_stage1\", \"peerclamp\"}},/\"droneguard\", \"droneguard_stage1\"}},/" quest-host-shared quest_config_test

# arena pays nothing, by count (the test counts SafeRead calls and level scans on every arena level): a read or a scan before the scope test is killed
runm pc_reads_before_scope $P "s/^  if (!scope::SeenCombatOrLobby()) {/  { bool okk = true; (void)SafeRead<std::uint32_t>(reinterpret_cast<std::uintptr_t>(bd), \&okk); }  if (!scope::SeenCombatOrLobby()) {/"
runm pc_scan_before_scope  $P "s/^#include \"quest\/combat\/safe_read.h\"/#include \"quest\/combat\/safe_read.h\"\n#include \"quest\/combat\/combat_level.h\"/; s/^  if (!scope::SeenCombatOrLobby()) {/  (void)level::Scan(reinterpret_cast<std::uintptr_t>(bd));  if (!scope::SeenCombatOrLobby()) {/"
# a true answer older than kStaleAfterMs is not believed
runm pc_stale_believed     $P "s/  if (!scope::LookIsFresh()) {  \/\/ the tick stopped.*/  if (false) {/"
# the scope must be able to change, or the hook is not installed
runm pc_live_unchecked     $P "s/  if (!scope::Live()) return Refused(\"scope_not_live\");//"
runm pc_live_after_pins    $P "s/  if (!scope::Live()) return Refused(\"scope_not_live\");/  (void)scope::Live();/"
# the scope row is on while this guard is (combat_install.cpp ScopeNormal)
runm pc_reader_unlisted    $CI "s/g_config.peerclamp.on || //" quest-host-combat combat_install_test
runm pc_row_member         $CI "s/\"combat.peerclamp\", \&nevr_quest::CombatConfig::peerclamp,/\"combat.peerclamp\", \&nevr_quest::CombatConfig::voipdrop,/" quest-host-combat combat_install_test
runm pc_row_key            $CI "s/\"combat.peerclamp\", \&nevr_quest/\"combat.peerclamps\", \&nevr_quest/" quest-host-combat combat_install_test
# every cap says when it stops (review, line_budget.h)
runm pc_clamp_cap_ignored   $P "s/  if (LineBudget(\&g_clampLines, kClampLines,/  if (true || LineBudget(\&g_clampLines, kClampLines,/"
runm pc_total_cap_ignored   $P "s/             LineBudget(\&g_totals, kTotalLines,/             LineBudget(\&g_totals, 1000000,/"
