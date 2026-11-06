# shaderstep: each rule of src/quest/combat/shaderstep.cpp, shaderstep_targets.h and the step correction in table_step.cpp removed or bent once;
# src/quest/tests/shaderstep_test.cpp must fail on every one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed>`.
S=src/quest/combat/shaderstep.cpp
T=src/quest/combat/shaderstep_targets.h
TS=src/quest/combat/table_step.cpp
TH=src/quest/combat/table_step.h
# the scope: the correction only in a combat match
runm ss_scope_inverted      $S "s/  if (scope != table_step::Scope::kCombat) {/  if (scope == table_step::Scope::kCombat) {/"
runm ss_always_corrects     $S "s/  if (scope != table_step::Scope::kCombat) {/  if (false) {/"
runm ss_never_corrects      $S "s/  if (scope != table_step::Scope::kCombat) {/  if (true) {/"
runm ss_scope_not_read      $S "s/  const table_step::Scope scope = table_step::CurrentScope();/  const table_step::Scope scope = table_step::Scope::kCombat;/"
# the game's function runs once, with the game's arguments, and its object is returned
runm ss_original_dropped    $S "s/  void\* const built = original(self, obj, init);/  (void)original; (void)init; void* const built = obj;/"
runm ss_original_args       $S "s/  void\* const built = original(self, obj, init);/  void* const built = original(self, obj, obj);/"
runm ss_other_object_back   $S "s/^  return built;\$/  return obj == built ? nullptr : built;/"
runm ss_stock_not_counted   $S "s/g_stock.fetch_add(1, std::memory_order_relaxed);/g_stock.fetch_add(0, std::memory_order_relaxed);/"
runm ss_stock_said_always   $S "s/  if (g_saidStock.exchange(true, std::memory_order_relaxed)) return;/  (void)g_saidStock;/"
runm ss_stock_scope_unnamed $S "s/table_step::ScopeName(scope)/table_step::ScopeName(scope == scope ? table_step::Scope::kCombat : scope)/"
# the correction of the object it returned
runm ss_constructed_count   $S "s/const std::uint32_t n = g_constructed.fetch_add(1, std::memory_order_relaxed) + 1;/const std::uint32_t n = g_constructed.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm ss_stepped_count       $S "s/g_stepped.fetch_add(1, std::memory_order_relaxed);/g_stepped.fetch_add(0, std::memory_order_relaxed);/"
runm ss_not_stock_count     $S "s/g_notStock.fetch_add(1, std::memory_order_relaxed) < kNotStockLines/g_notStock.fetch_add(0, std::memory_order_relaxed) < kNotStockLines/"
runm ss_not_stock_cap       $S "s/constexpr std::uint32_t kNotStockLines = 5;/constexpr std::uint32_t kNotStockLines = 6;/"
runm ss_not_stock_uncapped  $S "s/g_notStock.fetch_add(1, std::memory_order_relaxed) < kNotStockLines/true/"
runm ss_first_said_always   $S "s/      if (!g_saidFirst.exchange(true, std::memory_order_relaxed)) {/      if (true) {/"
runm ss_first_never         $S "s/      if (!g_saidFirst.exchange(true, std::memory_order_relaxed)) {/      if (false) {/"
runm ss_first_n             $S "s/                             {\"n\", n}, /                             {\"n\", n + 1}, /"
runm ss_first_step_text     $S "s/{\"step\", table_step::kShaderSetStep},/{\"step\", table_step::kShaderSetStockStep},/"
runm ss_not_stock_found     $S "s/{\"found\", sentinel::HexString(word, found)}/{\"found\", sentinel::HexString(word, table_step::kShaderSetStockStep)}/"
runm ss_not_stock_level     $S "s/sentinel::LogFields(sentinel::LogLevel::kWarn, \"combat_shaderstep_not_stock\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_shaderstep_not_stock\"/"
runm ss_first_level_debug   $S "s/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_shaderstep_first\"/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_shaderstep_first\"/"
# the step correction (shared with tablestep): 0x20 -> 1 at +0x98, read back
runm ts_step_offset         $TH "s/inline constexpr std::uintptr_t kShaderSetStepOffset = 0x98;/inline constexpr std::uintptr_t kShaderSetStepOffset = 0xa0;/"
runm ts_stock_step          $TH "s/inline constexpr std::uint64_t kShaderSetStockStep = 0x20;/inline constexpr std::uint64_t kShaderSetStockStep = 0x21;/"
runm ts_new_step            $TH "s/inline constexpr std::uint64_t kShaderSetStep = 1;/inline constexpr std::uint64_t kShaderSetStep = 2;/"
runm ts_unreadable_ok       $TS "s/^  if (!ok) return StepFix::kUnreadable;\$//"
runm ts_stock_unchecked     $TS "s/^  if (have != kShaderSetStockStep) return StepFix::kNotStock;\$//"
runm ts_stock_inverted      $TS "s/  if (have != kShaderSetStockStep) return StepFix::kNotStock;/  if (have == kShaderSetStockStep) return StepFix::kNotStock;/"
runm ts_found_not_reported  $TS "s/^  if (found != nullptr) \*found = have;\$/  (void)found;/"
runm ts_write_dropped       $TS "s/^  std::memcpy(reinterpret_cast<void*>(at), \&one, sizeof one);\$/  (void)one;/"
runm ts_readback_unchecked  $TS "s/  return backOk \&\& now == kShaderSetStep ? StepFix::kSet : StepFix::kUnreadable;/  (void)now; (void)backOk; return StepFix::kSet;/"
runm ts_readback_value      $TS "s/  return backOk \&\& now == kShaderSetStep ? StepFix::kSet : StepFix::kUnreadable;/  return backOk \&\& now == kShaderSetStockStep ? StepFix::kSet : StepFix::kUnreadable;/"
# the install: the build, the switch, once, the word
runm ss_off_ignored         $S "s/^  if (!on) return Done(/  if (!on \&\& false) return Done(/"
runm ss_second_step_runs    $S "s/  if (g_ran.exchange(true, std::memory_order_acq_rel)) return/  if (false) return/"
runm ss_no_expected_fn      $T "s/^  target.expectedOriginal = .*\$/  (void)base;/"
runm ss_expected_wrong      $T "s/base + pins.fn);/base + pins.fn + 4);/"
runm ss_slot_pin_off        $T "s/sentinel::pinned::LibR15PatchedBuildId(), pins.slot);/sentinel::pinned::LibR15PatchedBuildId(), pins.slot + 8);/"
runm ss_kind_jump_slot      $T "s/sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId()/sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId()/"
runm ss_slot_is_the_other   $T "s/inline constexpr Pins kPins = {0x1906f4c, 0x365ddc8};/inline constexpr Pins kPins = {0x1906f4c, 0x365ddd0};/"
runm ss_legacy_not_warned   $S "s/^  table_step::WarnLegacySwitch(.*\$//"
runm ss_legacy_file_name    $S "s/\"shaderstep.off\", \"combat.shaderstep: false\"/\"shaderstep.of\", \"combat.shaderstep: false\"/"
runm ss_legacy_key_name     $S "s/\"shaderstep.off\", \"combat.shaderstep: false\"/\"shaderstep.off\", \"combat.shaderstep: true\"/"
runm ss_refusal_not_error   $S "s/refused ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo/refused ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kInfo/"
runm ss_install_slot_text   $S "s/sentinel::HexString(slot, pins.slot)/sentinel::HexString(slot, pins.fn)/"
runm ss_installed_status    $S "s/return Done(\"installed\", \"ok\", \"installed\",/return Done(\"installed\", \"ok\", \"ok\",/"
