# sendguard: each rule of src/quest/combat/sendguard.cpp and sendguard_targets.h removed or bent once; src/quest/tests/sendguard_test.cpp
# must fail on every one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed>`.
S=src/quest/combat/sendguard.cpp
T=src/quest/combat/sendguard_targets.h
H=src/quest/combat/sendguard.h
# the decision: a named level the lookup does not find is dropped (the null-space drop), and only then
runm sg_drop_inverted      $S "s/return lookup(space, data\[0\]) == nullptr;/return lookup(space, data[0]) != nullptr;/"
runm sg_never_drops        $S "s/return lookup(space, data\[0\]) == nullptr;/return lookup(space, data[0]) == nullptr \&\& false;/"
runm sg_always_drops       $S "s/return lookup(space, data\[0\]) == nullptr;/return lookup(space, data[0]) == nullptr || true;/"
runm sg_drop_calls_stock   $S "/^    LogDrop(node);\$/{n;s/    return;/    original(context, data);/}"
# the level key (data[0]): the lookup is asked it, and -1 means the current space
runm sg_level_key_event    $S "s/return lookup(space, data\[0\]) == nullptr;/return lookup(space, data[1]) == nullptr;/"
runm sg_no_level_looked_up $S "s/  if (data == nullptr || data\[0\] == kNoLevel) return false;/  if (data == nullptr) return false;/"
runm sg_no_level_inverted  $S "s/  if (data == nullptr || data\[0\] == kNoLevel) return false;/  if (data == nullptr || data[0] != kNoLevel) return false;/"
runm sg_no_level_value     $S "s/kNoLevel = ~0ULL;/kNoLevel = ~0ULL - 1;/"
# the chain to the current space: each offset the stock function reads
runm sg_chain_offset       $S "s/kSpaceChain = 0x30;/kSpaceChain = 0x38;/"
runm sg_space_offset       $S "s/kSpaceOffset = 0x80;/kSpaceOffset = 0x88;/"
runm sg_chain_first_load   $S "s/Load(Load(static_cast<const unsigned char\*>(context), 0), 0);/Load(static_cast<const unsigned char*>(context), 0);/"
runm sg_chain_skips_second $S "s/x9 = Load(Load(x9, kSpaceChain), 0);/x9 = Load(x9, kSpaceChain);/"
# a null on the way, a null context or a null node is left to the stock function
runm sg_null_node          $S "s/  if (data == nullptr || data\[0\] == kNoLevel) return false;/  if (data\[0\] == kNoLevel) return false;/"
runm sg_null_chain         $S "s/  if (x9 == nullptr) return false;/  (void)x9;/"
# the original is called with the node's own arguments, and passes are counted
runm sg_arg_swap           $S "s/  original(context, data);/  original(const_cast<void*>(data), context);/"
runm sg_arg_null_data      $S "s/  original(context, data);/  original(context, nullptr);/"
runm sg_pass_not_counted   $S "s/g_passed.fetch_add(1, std::memory_order_relaxed);/g_passed.fetch_add(0, std::memory_order_relaxed);/"
# the log: every drop counted, the first kDropLines written, each key named
runm sg_drop_not_counted   $S "s/const unsigned n = g_dropped.fetch_add(1, std::memory_order_relaxed) + 1;/const unsigned n = g_dropped.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm sg_drop_lines_cap     $H "s/kDropLines = 8;/kDropLines = 9;/"
runm sg_drop_no_cap        $S "s/  if (n > kDropLines) return;/  (void)kDropLines;/"
runm sg_drop_level_key     $S "s/sentinel::HexString(level, data\[0\])/sentinel::HexString(level, data[1])/"
runm sg_drop_event_key     $S "s/sentinel::HexString(event, data\[1\])/sentinel::HexString(event, data[2])/"
runm sg_drop_actor_key     $S "s/sentinel::HexString(actor, data\[2\])/sentinel::HexString(actor, data[0])/"
runm sg_drop_level_info    $S "s/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_sendguard_dropped\"/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_sendguard_dropped\"/"
# the install: the slot must hold Enter itself, the pins gate it, the build ID gates it
runm sg_no_expected_word   $S "s/  target.expectedOriginal = reinterpret_cast<const void\*>(image.base + kEnterVaddr);/  (void)kEnterVaddr;/"
runm sg_expected_wrong     $S "s/image.base + kEnterVaddr);/image.base + kEnterVaddr + 4);/"
runm sg_pins_skipped       $S "s/if (const char\* const mismatch = FirstPinMismatch(base))/if (const char* const mismatch = (FirstPinMismatch(base), nullptr))/"
runm sg_last_pin_skipped   $S "s/for (std::size_t i = 0; i < kCodeCount; ++i) {/for (std::size_t i = 0; i + 1 < kCodeCount; ++i) {/"
runm sg_pin_words_cut      $S "s/pin.words \* sizeof(std::uint32_t)) != 0) return pin.name;/sizeof(std::uint32_t)) != 0) return pin.name;/"
runm sg_pin_name_lost      $S "s/return pin.name;/return \"pin\";/"
runm sg_build_id_unchecked $S "s/  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\", nullptr);//"
runm sg_no_build_id_ok     $S "s/  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\", nullptr);//"
runm sg_refusal_quiet      $S "s/ModuleResult Refused(const char\* why, const char\* pin) noexcept {/ModuleResult Refused(const char* why, const char* pin) noexcept {\n  return {false, why};/"
runm sg_refused_reports_ok $S "s/  return {installed, installed ? \"installed\" : sentinel::GotStatusName(status)};/  return {true, \"installed\"};/"
runm sg_refused_installed  $S "s/  return {false, why};/  return {true, why};/"
runm sg_install_level      $S "s/sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_sendguard_install\",/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_sendguard_install\",/"
# the target: the symbol, the slot, the pins' words
runm sg_slot_vaddr         $T "s/kEnterSlotVaddr = 0x36f7928ULL;/kEnterSlotVaddr = 0x36f7930ULL;/"
runm sg_unwind_as_slot     $T "s/kEnterSlotVaddr = 0x36f7928ULL;/kEnterSlotVaddr = 0x356a6b0ULL;/"
runm sg_enter_vaddr        $T "s/kEnterVaddr = 0x2725a80ULL;/kEnterVaddr = 0x2725a84ULL;/"
runm sg_stub_vaddr         $T "s/kGameSpaceMtStubVaddr = 0xef5650ULL;/kGameSpaceMtStubVaddr = 0xef5654ULL;/"
runm sg_symbol             $T "s/CSendEventNode5Enter/CSendEventNode5Leave/"
runm sg_pin_word           $T "s/{0xf9404133, 0, 0, 0, 0, 0}, 1}/{0xf9404134, 0, 0, 0, 0, 0}, 1}/"
runm sg_pin_stub_word      $T "s/{0xb0013ef0, 0xf9410a11, 0x91084210, 0xd61f0220, 0, 0}, 4}/{0xb0013ef0, 0xf9410a11, 0x91084210, 0xd61f0221, 0, 0}, 4}/"
runm sg_pin_row_dropped    $T "s/    {\"Enter: bl GameSpaceMT\", 0x2725abcULL, {0x979f3ee5, 0, 0, 0, 0, 0}, 1},//"
# the switch and the old switch file
runm sg_off_ignored        $S "s/  if (!enabled) {/  if (!enabled \&\& false) {/"
runm sg_off_inverted       $S "s/  if (!enabled) {/  if (enabled) {/"
runm sg_off_reports_failed $S "s/    return {true, \"off_config\"};/    return {false, \"off_config\"};/"
runm sg_off_not_logged     $S "s/{\"result\", \"off\"},/{\"result\", \"on\"},/"
runm sg_legacy_not_told    $S "s/  if (LegacySwitchPresent(legacySwitchDir)) {/  if (false \&\& LegacySwitchPresent(legacySwitchDir)) {/"
runm sg_legacy_always      $S "s/ \&\& access(path, F_OK) == 0;/;/"
runm sg_legacy_turns_off   $S "s/  if (!enabled) {/  if (!enabled || LegacySwitchPresent(legacySwitchDir)) {/"
runm sg_legacy_quiet       $S "s/sentinel::LogLevel::kWarn, \"combat_sendguard_legacy_switch\"/sentinel::LogLevel::kInfo, \"combat_sendguard_legacy_switch\"/"
runm sg_row_ignores_config $S "s/combat::Config().sendguard.on/true/"
