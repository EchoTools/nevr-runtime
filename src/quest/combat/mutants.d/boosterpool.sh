# module `boosterpool`: each line removes one rule from src/quest/combat/boosterpool.cpp (or its targets header, or the row that reads its inputs); its tests
# (src/quest/tests/boosterpool_test.cpp, combat_install_test.cpp) must fail on it. Run: src/quest/combat/mutants.sh boosterpool
B=src/quest/combat/boosterpool.cpp
T=src/quest/combat/boosterpool_targets.h
runm bp_no_substitution  $B "s/  const ActorPair actor = original(st, plain);/  const ActorPair actor = original(st, name);/"
runm bp_always_substitute $B "s/  if (name != suffixed) return original(st, name);/  (void)0;/"
runm bp_plain_ignored    $B "s/  \*suffixed = append(\*plain, pin::kSuffix, 1, 1);/  *suffixed = append(0, pin::kSuffix, 1, 1);/"
runm bp_append_flags     $B "s/append(\*plain, pin::kSuffix, 1, 1)/append(*plain, pin::kSuffix, 1, 0)/"
runm bp_suffix           $T "s/kSuffix\[\] = \"_lowspec\";/kSuffix[] = \"_lowspeC\";/"
# arena pays nothing, by count (AnArenaCallMakesNoReadAfterTheFirstAndNoScan): the names read again on every call, and a scan before the name is compared
runm bp_arena_read_every_call $B "s/  if (suffixed != 0) {/  if (false) {/"
runm bp_arena_scan_first $B "s|^  std::uint64_t suffixed = g_suffixedName.load(std::memory_order_acquire);|  (void)nevr_quest::combat::level::Scan(0);\n&|;s|^#include \"quest/combat/boosterpool.h\"|&\n#include \"quest/combat/combat_level.h\"|"
runm bp_no_unreadable_guard $B "s/  if (!SafeRead(g_plainAddress.load(std::memory_order_acquire), plain, sizeof(\*plain))) {/  if (false) {/"
runm bp_unreadable_uncounted $B "s/    g_unreadable.fetch_add(1, std::memory_order_relaxed);/    (void)0;/"
runm bp_no_append_guard  $B "s/  if (append == nullptr) return false;/  (void)0;/"
runm bp_fn_vaddr         $B "s/image.base + pin::kFnVaddr/image.base + pin::kFnVaddr + 0x10/"
runm bp_slot_vaddr       $T "s/kSlotVaddr = 0x36cd1f8;/kSlotVaddr = 0x36cd1f0;/"
runm bp_plain_vaddr      $B "s/image.base + pin::kPlainNameVaddr/image.base + pin::kPlainNameVaddr + 8/"
runm bp_append_vaddr     $B "s/image.base + pin::kAppendStubVaddr/image.base + pin::kAppendStubVaddr + 4/"
runm bp_kept_counter     $B "s/  const std::uint64_t n = g_kept.fetch_add(1, std::memory_order_relaxed) + 1;/  const std::uint64_t n = 1;/"
runm bp_log_unbounded    $B "s/  if (!LineBudget(\&g_keptLines, kLogFirstKept,/  if (!LineBudget(\&g_keptLines, 1000000,/"
runm bp_no_kept_log      $B "s/  if (!LineBudget(\&g_keptLines, kLogFirstKept,/  if (!LineBudget(\&g_keptLines, 0,/"
runm bp_stats_dropped    $B "s/  if (g_installed.load(std::memory_order_acquire)) LogStats();/  if (false) LogStats();/"
runm bp_stats_level      $B "s/sentinel::LogLevel::kInfo, \"combat_boosterpool_stats\"/sentinel::LogLevel::kDebug, \"combat_boosterpool_stats\"/"
runm bp_off_not_counted  $B "s/    return {false, \"off\", true};/    return {false, \"off\"};/" quest-host-combat combat_install_test
runm bp_kept_level       $B "s/sentinel::LogLevel::kInfo, \"combat_boosterpool_kept\"/sentinel::LogLevel::kDebug, \"combat_boosterpool_kept\"/"
runm bp_cache_not_stored $B "s/    g_suffixedName.store(\*suffixed, std::memory_order_release);.*/    (void)0;/"
runm bp_cache_zero_plain $B "s/  if (\*plain != 0 \&\& \*suffixed != 0) {/  if (*suffixed != 0) {/"
runm bp_cached_plain     $B "s/    plain = g_plainName.load(std::memory_order_acquire);/    plain = 0;/"
runm bp_plain_not_cached $B "s/    g_plainName.store(\*plain, std::memory_order_release);/    (void)0;/"
runm bp_expected_unchecked $B "s/target.expectedOriginal = reinterpret_cast<const void\*>(image.base + pin::kFnVaddr);.*/(void)image;/"
runm bp_stays_armed      $B "s/    Thunk::Disarm();\$/    (void)0;/"
runm bp_never_armed      $B "s/  Thunk::Arm(kRecord);/  (void)kRecord;/"
runm bp_installed_unset  $B "s/  g_installed.store(true, std::memory_order_release);/  (void)0;/"
runm bp_twice_ok         $B "s/  if (g_installed.load(std::memory_order_acquire)) {/  if (false) {/"
runm bp_off_installs     $B "s/  if (!on) {/  if (!on \&\& false) {/"
runm bp_needs_levelname  $B "s/  if (!levelnameInstalled) {/  if (!levelnameInstalled \&\& false) {/"
runm bp_off_before_needs $B "s/  if (!on) {/  if (!on \&\& levelnameInstalled) {/"
runm bp_refusal_quiet    $B "s/std::strcmp(result, \"refused\") == 0 ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo/sentinel::LogLevel::kInfo/"
runm bp_no_lib_check     $B "s/  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, \&image)) {/  if (false) {/"
CI=src/quest/combat/combat_install.cpp
runm ci_booster_switch   $CI "s/boosterpool::Install(g_config.levelname.on, levelname::Installed())/boosterpool::Install(true, levelname::Installed())/" quest-host-combat combat_install_test
runm ci_booster_needs    $CI "s/boosterpool::Install(g_config.levelname.on, levelname::Installed())/boosterpool::Install(g_config.levelname.on, true)/" quest-host-combat combat_install_test
runm ci_booster_row      $CI "s/    Module{\"boosterpool\", nullptr, \&BoosterPoolNormal, nullptr, nullptr, \&boosterpool::NoteEnd},/    Module{\"boosterpool\", \&BoosterPoolNormal, nullptr, nullptr, nullptr, \&boosterpool::NoteEnd},/" quest-host-combat combat_install_test
runm ci_booster_end      $CI "s/, nullptr, nullptr, \&boosterpool::NoteEnd},/, nullptr, nullptr, nullptr},/" quest-host-combat combat_install_test
