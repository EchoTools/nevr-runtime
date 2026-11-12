# heaps: each rule of src/quest/combat/heaps.cpp and heaps_targets.h removed or bent once; src/quest/tests/heaps_test.cpp must fail on every one
# (heap_step_test.cpp for the two that only the shipped table sees). The `hrow_` mutants edit the install table (src/quest/combat/combat_install.cpp,
# a shared file) and are run against heap_step_test and combat_install_test: run them apart from the others (`mutants.sh heaps '^h_'`, then
# `mutants.sh heaps '^hrow_'` while holding the shared lock). Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
#
# Not mutated, because no run can tell the mutant from the original:
#   - `!readerRead ||` and `!stockRead ||` in SwapRow: a failed read leaves the word 0, which differs from the pin, so the row is refused either way
#     (and the line's `have` comes from the separate `readable` argument);
#   - `!ReadGame(slot) || !ReadGame(pointer)` to `&&` in ObservedMiB: a failed pointer read leaves 0 bytes, which is the answer for a failed read;
#   - `/*write=*/false` to `true` in ReadGame: the test image is writable wherever it is readable;
#   - the `return;` after the device override in ReadDevice: the code after it is the Android branch, which is not compiled on a host;
#   - `.Owned("combat_heaps")` in RowTarget: a name for the chain the manager shows on a refusal of a hook, which a data write never makes;
#   - the test seams and ResetForTest.
F=src/quest/combat/heaps.cpp
T=src/quest/combat/heaps_targets.h
CI=src/quest/combat/combat_install.cpp
# the pins: each row's symbol, slot, .rodata word, reader, reader word and sizes
for row in "R06 kResourceMemoryAndroidE 370b720 2b9caa0 f78590 206 320" "R07 kSystemMemoryAndroidE 371d020 2b9ca90 f78524 400 640" \
           "R08 kGpuMemoryAndroidE 3710188 2b9ca98 f78614 160 512" "R09 kR15GameSpaceMemoryQuestE 3715198 2b9e1c0 11ffdec 40 128"; do
  read -r id sym slot ro reader stock target <<<"$row"
  runm h_pin_${id}_symbol $T "/{\"$id\",/s/$sym/${sym%E}X/"
  runm h_pin_${id}_slot   $T "/{\"$id\",/s/ 0x${slot}ULL/ 0x$(printf '%x' $((0x$slot + 1)))ULL/"
  runm h_pin_${id}_ro     $T "/{\"$id\",/s/ 0x${ro}ULL/ 0x$(printf '%x' $((0x$ro + 1)))ULL/"
  runm h_pin_${id}_reader $T "/{\"$id\",/s/ 0x${reader}ULL/ 0x$(printf '%x' $((0x$reader + 4)))ULL/"
  runm h_pin_${id}_stock  $T "/{\"$id\",/s/ $stock \* kMiB/ $((stock + 1)) * kMiB/"
  runm h_pin_${id}_target $T "/{\"$id\",/s/ $target \* kMiB}/ $((target + 1)) * kMiB}/"
done
runm h_pin_R06_reader_word $T "/{\"R06\",/s/kLdrX1FromX8/kLdrX0FromX8/"
runm h_pin_R09_reader_word $T "/{\"R09\",/s/kLdrX0FromX8/kLdrX1FromX8/"
runm h_pin_ldr_x1          $T "s/kLdrX1FromX8 = 0xf9400101;/kLdrX1FromX8 = 0xf9400102;/"
runm h_pin_ldr_x0          $T "s/kLdrX0FromX8 = 0xf9400100;/kLdrX0FromX8 = 0xf9400104;/"
runm h_pin_mib             $T "s/kMiB = 1ULL << 20;/kMiB = 1ULL << 21;/"
runm h_pin_system_row      $T "s/kSystemRow = 1;/kSystemRow = 2;/"
runm h_target_kind         $T "s/sentinel::RelocKind::kGlobDat,/sentinel::RelocKind::kJumpSlot,/"
runm h_target_build_id     $T "s/sentinel::pinned::LibR15PatchedBuildId(), row.slot)/sentinel::pinned::kLibR15StockBuildId, row.slot)/"
runm h_target_slot         $T "s/sentinel::pinned::LibR15PatchedBuildId(), row.slot)/sentinel::pinned::LibR15PatchedBuildId(), row.ro)/"
# a read of the game's memory is checked against the mappings first
runm h_read_unchecked      $F "s/^  if (!sentinel::MappingsAllow(address, sizeof(T), \\/\\*write=\\*\\/false)) return false;//"
# the headset's name: the seam, then the copy
runm h_dev_override_ignored $F "s/  if (const char\\* const device = g_deviceOverride.load(std::memory_order_acquire)) {/  if (const char* const device = static_cast<const char*>(nullptr)) {/"
runm h_dev_copy            $F "s/    std::snprintf(out, size, \"%s\", device);/    std::snprintf(out, size, \"%s\", \"x\");/"
# what the game reads through a slot now
runm h_obs_unit            $F "s/  return bytes \\/ kMiB;/  return bytes;/"
runm h_obs_slot            $F "s/ReadGame(base + kRows\\[row\\].slot, \\&pointer)/ReadGame(base + kRows[row].ro, \\&pointer)/"
# the row line
runm h_row_id              $F "s/{{\"row\", row.id}, {\"status\", status}/{{\"row\", row.symbol}, {\"status\", status}/"
runm h_row_status_why      $F "s/{\"status\", status}, {\"why\", why}, {\"slot_vaddr\"/{\"status\", why}, {\"why\", status}, {\"slot_vaddr\"/"
runm h_row_slot            $F "s/{\"status\", status}, {\"why\", why}, {\"slot_vaddr\", sentinel::HexString(slot, row.slot)}/{\"status\", status}, {\"why\", why}, {\"slot_vaddr\", sentinel::HexString(slot, row.ro)}/"
runm h_row_value           $F "s/{\"value_mib\", valueBytes \\/ kMiB}, {\"observed_mib\", observedMiB}, {\"device\", device}});/{\"value_mib\", valueBytes}, {\"observed_mib\", observedMiB}, {\"device\", device}});/"
runm h_row_observed        $F "s/{\"observed_mib\", observedMiB}/{\"observed_mib\", observedMiB * 0}/"
runm h_row_device          $F "s/{\"observed_mib\", observedMiB}, {\"device\", device}});/{\"observed_mib\", observedMiB}, {\"device\", device[0] ? \"\" : device}});/"
# the refusal of a pinned word that is not the stock one (logging.md Rule 6)
runm h_word_level          $F "s/sentinel::LogFields(sentinel::LogLevel::kWarn, \"combat_heaps_row\",/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_heaps_row\",/"
runm h_word_slot           $F "s/{\"status\", \"refused\"}, {\"why\", why}, {\"slot_vaddr\", sentinel::HexString(slot, row.slot)}/{\"status\", \"refused\"}, {\"why\", why}, {\"slot_vaddr\", sentinel::HexString(slot, row.ro)}/"
runm h_word_readable       $F "s/{\"have\", readable ? sentinel::HexString(haveText, have) : \"unreadable\"}/{\"have\", readable || true ? sentinel::HexString(haveText, have) : \"unreadable\"}/"
runm h_word_want           $F "s/{\"want\", sentinel::HexString(wantText, want)}/{\"want\", sentinel::HexString(wantText, want * 0 + have)}/"
runm h_word_value          $F "s/{\"want\", sentinel::HexString(wantText, want)}, {\"value_mib\", valueBytes \\/ kMiB}/{\"want\", sentinel::HexString(wantText, want)}, {\"value_mib\", valueBytes}/"
runm h_word_device         $F "s/{\"value_mib\", valueBytes \\/ kMiB}, {\"device\", device}});/{\"value_mib\", valueBytes \\/ kMiB}, {\"device\", device[0] ? \"\" : device}});/"
# one row: the reader word, the .rodata size, our value, the write, the lines
runm h_swap_reader_cmp     $F "s/readerWord != pin.readerWord/readerWord == pin.readerWord/"
runm h_swap_reader_args    $F "s/readerRead, readerWord, pin.readerWord, valueBytes/readerRead, pin.readerWord, readerWord, valueBytes/"
runm h_swap_reader_why     $F "s/\"reader_word_not_stock\"/\"reader_word\"/"
runm h_swap_stock_cmp      $F "s/stockBytes != pin.stock/stockBytes == pin.stock/"
runm h_swap_stock_args     $F "s/stockRead, stockBytes, pin.stock, valueBytes/stockRead, pin.stock, stockBytes, valueBytes/"
runm h_swap_stock_why      $F "s/\"rodata_not_stock\"/\"rodata\"/"
runm h_swap_value_unpublished $F "s/^  g_value\\[row\\] = valueBytes;.*\$/  (void)0;/"
runm h_swap_value_row0     $F "s/\\&g_value\\[row\\], lookup);/\\&g_value[0], lookup);/"
runm h_swap_expected       $F "s/reinterpret_cast<const void\\*>(image.base + pin.ro),/reinterpret_cast<const void*>(image.base + pin.ro + 8),/"
runm h_swap_status_cmp     $F "s/  if (status != sentinel::GotStatus::kOk) {/  if (status == sentinel::GotStatus::kOk) {/"
runm h_swap_refused_level  $F "s/LogRow(sentinel::LogLevel::kError, pin, \"refused\"/LogRow(sentinel::LogLevel::kWarn, pin, \"refused\"/"
runm h_swap_refused_observed $F "s/sentinel::GotStatusName(status), valueBytes, 0, device);/sentinel::GotStatusName(status), valueBytes, 1, device);/"
runm h_swap_swapped_level  $F "s/LogRow(sentinel::LogLevel::kDebug, pin, \"swapped\"/LogRow(sentinel::LogLevel::kInfo, pin, \"swapped\"/"
runm h_swap_swapped_observed $F "s/\"slot_points_at_our_value\", valueBytes, ObservedMiB(image.base, row), device);/\"slot_points_at_our_value\", valueBytes, 0, device);/"
runm h_swap_returns        $F "/^bool SwapRow/,/^}/s/^  return true;/  return false;/"
# the install line: its level, each size read back, the counts, the consequence
runm h_inst_ok_no_refused  $F "s/  const bool ok = counts.refused == 0 \\&\\& std::strcmp(result, \"refused\") != 0;/  const bool ok = std::strcmp(result, \"refused\") != 0;/"
runm h_inst_ok_no_result   $F "s/  const bool ok = counts.refused == 0 \\&\\& std::strcmp(result, \"refused\") != 0;/  const bool ok = counts.refused == 0;/"
runm h_inst_level          $F "s/sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_heaps_install\"/sentinel::LogFields(ok ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, \"combat_heaps_install\"/"
runm h_inst_base_guard     $F "s/{\"resource_mib\", base != 0 ? ObservedMiB(base, 0) : 0}/{\"resource_mib\", base == 0 ? ObservedMiB(base, 0) : 0}/"
runm h_inst_resource_row   $F "s/ObservedMiB(base, 0) : 0}/ObservedMiB(base, 1) : 0}/"
runm h_inst_system_row     $F "s/ObservedMiB(base, kSystemRow)/ObservedMiB(base, 0)/"
runm h_inst_gpu_row        $F "s/ObservedMiB(base, 2)/ObservedMiB(base, 3)/"
runm h_inst_game_space_row $F "s/ObservedMiB(base, 3)/ObservedMiB(base, 2)/"
runm h_inst_swapped        $F "s/{\"swapped\", counts.swapped}/{\"swapped\", counts.refused}/"
runm h_inst_refused        $F "s/{\"refused\", counts.refused}/{\"refused\", counts.swapped}/"
runm h_inst_left           $F "s/{\"left\", counts.left}/{\"left\", counts.refused}/"
runm h_inst_result_reason  $F "s/{{\"result\", result}, {\"reason\", reason}/{{\"result\", reason}, {\"reason\", result}/"
runm h_inst_device         $F "s/{\"device\", device}, {\"consequence\", consequence}});/{\"device\", device[0] ? \"\" : device}, {\"consequence\", consequence}});/"
runm h_inst_consequence    $F "s/{\"consequence\", consequence}});/{\"consequence\", consequence[0] ? \"\" : consequence}});/"
runm h_stock_text          $F "s/(206, 400, 160 and 40 MiB)\";/(206, 401, 160 and 40 MiB)\";/"
runm h_refused_result      $F "s/  LogInstall(\"refused\", token, none, 0, device, kStockConsequence);/  LogInstall(\"off\", token, none, 0, device, kStockConsequence);/"
runm h_refused_status      $F "s/  return {false, token};/  return {false, \"refused\"};/"
# the System heap follows the headset profile
runm h_sys_zero_profile    $F "s/  if (match.profile->systemHeapMiB > 0) return/  if (match.profile->systemHeapMiB >= 0) return/"
runm h_sys_unit            $F "s/static_cast<std::uint64_t>(match.profile->systemHeapMiB) \\* kMiB;/static_cast<std::uint64_t>(match.profile->systemHeapMiB) * 1024;/"
runm h_sys_default         $F "s/  return kRows\\[kSystemRow\\].target;/  return kRows[0].target;/"
runm h_sys_null            $F "s/IdentifyDevice(device != nullptr ? device : \"\");/IdentifyDevice(device);/"
# the step: off, once, the image, the build ID
runm h_null_device         $F "s/^  if (device == nullptr) device = \"\";//"
runm h_off_inverted        $F "s/  if (!heapsOn) {/  if (heapsOn) {/"
runm h_off_flag            $F "s/    return {false, \"off\", true};/    return {false, \"off\", false};/" quest-host-combat heap_step_test
runm h_off_reason          $F "s/LogInstall(\"off\", \"combat.heaps\", none/LogInstall(\"off\", \"combat.heap\", none/"
runm h_ran_not_set         $F "s/g_ran.exchange(true, std::memory_order_acq_rel)/g_ran.load(std::memory_order_acquire)/"
runm h_ran_level           $F "s/sentinel::LogLevel::kWarn, \"combat_heaps_refused\"/sentinel::LogLevel::kInfo, \"combat_heaps_refused\"/"
runm h_ran_status          $F "s/    return {false, \"already_ran\"};/    return {false, \"off\"};/"
runm h_ran_why             $F "s/{{\"why\", \"already_ran\"}, /{{\"why\", \"already\"}, /"
runm h_lookup_null         $F "s/  if (lookup == nullptr || !lookup(pinned::kLibR15, \\&image)) return/  if (!lookup(pinned::kLibR15, \\&image)) return/"
runm h_lookup_inverted     $F "s/  if (lookup == nullptr || !lookup(pinned::kLibR15, \\&image)) return/  if (lookup == nullptr || lookup(pinned::kLibR15, \\&image)) return/"
runm h_build_id_unread     $F "s/^  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\", device);//"
runm h_build_id_cmp        $F "s/std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0/std::strcmp(id, pinned::LibR15PatchedBuildId()) == 0/"
runm h_build_id_stock      $F "s/std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0/std::strcmp(id, pinned::kLibR15StockBuildId) != 0/"
# the loop: the System row left alone on combat.memsys false, the value each row gets
runm h_left_no_system      $F "s/    if (row == kSystemRow \\&\\& !systemOn) {/    if (!systemOn) {/"
runm h_left_no_switch      $F "s/    if (row == kSystemRow \\&\\& !systemOn) {/    if (row == kSystemRow \\&\\& (!systemOn || true)) {/"
runm h_left_uncounted      $F "s/      ++counts.left;/      (void)counts;/"
runm h_left_no_continue    $F "/row == kSystemRow \\&\\& !systemOn/,/continue;/s/^      continue;/      (void)0;/"
runm h_left_level          $F "s/LogRow(sentinel::LogLevel::kInfo, kRows\\[row\\], \"left\"/LogRow(sentinel::LogLevel::kDebug, kRows[row], \"left\"/"
runm h_left_why            $F "s/\"combat.memsys_is_false\"/\"combat.memsys\"/"
runm h_left_value          $F "s/\"combat.memsys_is_false\", kRows\\[row\\].stock,/\"combat.memsys_is_false\", kRows[row].target,/"
runm h_value_row           $F "s/    const std::uint64_t value = row == kSystemRow ?/    const std::uint64_t value = row != kSystemRow ?/"
runm h_value_headset       $F "s/? SystemBytesFor(device) : kRows\\[row\\].target;/? kRows[row].target : kRows[row].target;/"
runm h_count_swapped       $F "s/    if (SwapRow(image, lookup, row, value, device)) {/    if (!SwapRow(image, lookup, row, value, device)) {/"
runm h_result_partial      $F "s/(counts.swapped > 0 ? \"partial\" : \"refused\");/(counts.swapped < 4 ? \"partial\" : \"refused\");/"
runm h_result_cmp          $F "s/const char\\* const result = counts.refused == 0 ?/const char* const result = counts.refused != 0 ?/"
runm h_reason              $F "s/counts.refused == 0 ? \"ok\" : \"row_refused\"/counts.refused == 0 ? \"row_refused\" : \"ok\"/"
runm h_consequence_ok      $F "s/\"the game's heaps are the raised sizes when android_main reads them\"/\"the heaps are raised\"/"
runm h_consequence_partial $F "s/\"a refused row keeps the game's stock size; the others are raised\"/\"a refused row keeps its stock size\"/"
runm h_consequence_swapped $F "s/(counts.swapped > 0 ? \"a refused row keeps/(counts.swapped > 3 ? \"a refused row keeps/"
runm h_installed_flag      $F "s/  return {counts.refused == 0, result};/  return {counts.swapped > 0, result};/"
# the entry the sentinel calls: the seam for the lookup, the device name
runm h_entry_lookup        $F "s/InstallAt(lookup != nullptr ? lookup : \\&sentinel::FindLoadedImage, device, heapsOn, systemOn);/InstallAt(lookup != nullptr ? \\&sentinel::FindLoadedImage : \\&sentinel::FindLoadedImage, device, heapsOn, systemOn);/"
runm h_entry_device        $F "s/^  ReadDevice(device, sizeof(device));/  ReadDevice(device, sizeof(device));\n  device[0] = '\\\\0';/"
runm h_entry_flags         $F "s/device, heapsOn, systemOn);\$/device, systemOn, heapsOn);/"
# the table row (combat_install.cpp, shared) and its tests: each key reaches the module, both rows are early, heaps first
runm hrow_heaps_key        $CI "s/{ return heaps::Install(g_config.heaps.on, g_config.memsys.on); }/{ return heaps::Install(true, g_config.memsys.on); }/" quest-host-combat heap_step_test
runm hrow_system_key       $CI "s/{ return heaps::Install(g_config.heaps.on, g_config.memsys.on); }/{ return heaps::Install(g_config.heaps.on, true); }/" quest-host-combat heap_step_test
runm hrow_keys_swapped     $CI "s/{ return heaps::Install(g_config.heaps.on, g_config.memsys.on); }/{ return heaps::Install(g_config.memsys.on, g_config.heaps.on); }/" quest-host-combat heap_step_test
runm hrow_early_slot       $CI "s/Module{\"heaps\", \\&HeapsEarly, nullptr}/Module{\"heaps\", nullptr, \\&HeapsEarly}/" quest-host-combat combat_install_test
runm hrow_name             $CI "s/Module{\"heaps\", \\&HeapsEarly, nullptr}/Module{\"heap\", \\&HeapsEarly, nullptr}/" quest-host-combat combat_install_test
