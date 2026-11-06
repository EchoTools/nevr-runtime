# module `levelname`: each line removes one rule from src/quest/combat/levelname.cpp (or its targets header, or the rows that read its switch); its tests
# (src/quest/tests/levelname_test.cpp, combat_install_test.cpp, quest_config_test.cpp) must fail on it. Run: src/quest/combat/mutants.sh levelname
L=src/quest/combat/levelname.cpp
T=src/quest/combat/levelname_targets.h
runm ln_key_wrong        $T "s/kGlobalMp = 0x3F9915D3001DC28EULL;/kGlobalMp = 0x3F9915D3001DC28FULL;/"
runm ln_any_name         $L "s/  if (name == pin::kGlobalMp) {/  if (true) {/"
runm ln_no_flag_test     $L "s/(flags \& pin::kLowspecMask) != 0/flags != 0x5a5a5a5aU/"
runm ln_bit30            $L "s/(flags \& pin::kLowspecMask) != 0/(flags \& 0x40000000U) != 0/"
runm ln_any_bit          $L "s/(flags \& pin::kLowspecMask) != 0/flags != 0/"
runm ln_flag_offset      $L "s/reinterpret_cast<std::uintptr_t>(game) + pin::kFlagOffset, \&ok)/reinterpret_cast<std::uintptr_t>(game) + pin::kFlagOffset + 4, \&ok)/"
runm ln_unsafe_read      $L "s/const auto flags = SafeRead<std::uint32_t>(reinterpret_cast<std::uintptr_t>(game) + pin::kFlagOffset, \&ok);/const auto flags = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(game) + pin::kFlagOffset);/"
# not listed: `if (ok && (flags & mask) != 0)` without the `ok` is the same program, because a read that failed leaves the flags 0 (combat/safe_read.h)
runm ln_plain_stub       $L "s/image.base + pin::kPlainStubVaddr/image.base + pin::kPlainStubVaddr + 0x10/"
runm ln_fn_vaddr         $L "s/image.base + pin::kFnVaddr/image.base + pin::kFnVaddr + 4/"
runm ln_plain_args       $L "s/plain(game, name);/plain(game, 0);/"
runm ln_plain_bypassed   $L "s/const std::uint64_t answer = plain(game, name);/const std::uint64_t answer = name;/"
runm ln_real_args        $L "s/  return original(game, name);/  return original(game, name + 1);/"
runm ln_real_game        $L "s/  return original(game, name);/  return original(nullptr, name);/"
runm ln_plain_null_ok    $L "s/ \&\& plain != nullptr) {/) {/"
runm ln_counter_wrong    $L "s/g_kept.fetch_add(1, std::memory_order_relaxed) + 1;/g_kept.fetch_add(2, std::memory_order_relaxed) + 2;/"
runm ln_log_unbounded    $L "s/  if (n > kLogFirstKept) return;/  (void)kLogFirstKept;/"
runm ln_no_kept_log      $L "s/  if (n > kLogFirstKept) return;/  if (n > 0) return;/"
runm ln_kept_level       $L "s/sentinel::LogLevel::kInfo, \"combat_levelname_kept\"/sentinel::LogLevel::kDebug, \"combat_levelname_kept\"/"
runm ln_slot0            $T "s/kSlots\[kSlotCount\] = {0x362ef90, /kSlots[kSlotCount] = {0x362ef98, /"
runm ln_slot1            $T "s/0x362ef90, 0x3630e58, /0x362ef90, 0x3630e50, /"
runm ln_slot2            $T "s/0x3630e58, 0x3631278, /0x3630e58, 0x3631270, /"
runm ln_slot3            $T "s/0x3631278, 0x36316f8}/0x3631278, 0x36316f0}/"
runm ln_reloc_kind       $T "s/sentinel::RelocKind::kAbs64/sentinel::RelocKind::kJumpSlot/"
runm ln_last_slot_skipped $L "s/r.status\[i\] = kOps\[i\].put(target, lookup);/r.status[i] = i == 3 ? sentinel::GotStatus::kOk : kOps[i].put(target, lookup);/"
runm ln_expected_unchecked $L "s/target.expectedOriginal = reinterpret_cast<const void\*>(image.base + pin::kFnVaddr);.*/(void)image;/"
runm ln_never_armed      $L "s/    kOps\[i\].arm();/    (void)i;/;s/for (std::size_t i = 0; i < pin::kSlotCount; ++i) kOps\[i\].arm();/(void)0;/"
runm ln_no_rollback      $L "s/      if (!g_hooks\[i\].installed()) continue;/      continue;/"
runm ln_rollback_count   $L "s/        ++r.rolledBack;/        r.rolledBack += 2;/"
runm ln_rollback_fail_count $L "s/        ++r.rollbackFailed;/        r.rollbackFailed += 0;/"
runm ln_stays_armed      $L "s/    for (std::size_t i = 0; i < pin::kSlotCount; ++i) kOps\[i\].disarm();/    (void)0;/"
runm ln_plain_kept       $L "s/    g_plain.store(0, std::memory_order_release);/    (void)0;/"
runm ln_continue_on_refusal $L "s/      r.failedSlot = static_cast<int>(i);/      r.failedSlot = static_cast<int>(i);\n      continue;/"
runm ln_installed_unset  $L "s/  g_installed.store(true, std::memory_order_release);/  (void)0;/"
runm ln_twice_ok         $L "s/  if (g_installed.load(std::memory_order_acquire)) {/  if (false) {/"
runm ln_off_installs     $L "s/  if (!on) {/  if (!on \&\& false) {/"
runm ln_refusal_quiet    $L "s/bad ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo/bad ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kInfo/"
runm ln_off_reason       $L "s/\"off_config\"/\"off\"/"
runm ln_legacy_quiet     $L "s/sentinel::LogLevel::kWarn, \"combat_levelname_legacy_switch\"/sentinel::LogLevel::kInfo, \"combat_levelname_legacy_switch\"/"
runm ln_legacy_ignored   $L "s/  if (access(path, F_OK) != 0) return;/  return;/"
runm ln_failed_slot_name $L "s/r.failedSlot >= 0 ? pin::kSlotNames\[r.failedSlot\] : \"none\"/\"none\"/"
runm ln_no_lib_check     $L "s/  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, \&image)) {/  if (false) {/"
# the rows and the key that reach the module (combat_install.cpp, quest_config.cpp, nevr_config.cpp)
CI=src/quest/combat/combat_install.cpp
runm ci_levelname_switch $CI "s/levelname::Install(g_config.levelname.on)/levelname::Install(true)/" quest-host-combat combat_install_test
runm ci_levelname_row    $CI "s/    Module{\"levelname\", nullptr, \&LevelNameNormal},/    Module{\"levelname\", \&LevelNameNormal, nullptr},/" quest-host-combat combat_install_test
runm ci_config_line      $CI "s/{\"levelname\", config.levelname.on ? 1 : 0}, {\"levelname_source\", source(config.levelname.source)},//" quest-host-combat combat_install_test
QC=src/quest/sentinel/quest_config.cpp
runm qc_levelname_read   $QC "s/  r.config.combat.levelname = ReadCombatSwitch(cfg, kCombatLevelNameKey, r);/  (void)kCombatLevelNameKey;/" quest-host-shared quest_config_test
runm qc_levelname_key    $QC "s/kCombatLevelNameKey = \"combat.levelname\";/kCombatLevelNameKey = \"combat.levelnames\";/" quest-host-shared quest_config_test
runm qc_levelname_default $QC "s/  r.config.combat.levelname = ReadCombatSwitch(cfg, kCombatLevelNameKey, r);/  r.config.combat.levelname = ReadCombatSwitch(cfg, kCombatLevelNameKey, r, false);/" quest-host-shared quest_config_test
runm nc_levelname_known  src/core/nevr_config.cpp "s/\"refresh_rate_quest2_72\", \"levelname\"}}/\"refresh_rate_quest2_72\"}}/" quest-host-shared quest_config_test
