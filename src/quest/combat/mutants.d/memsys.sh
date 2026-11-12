# memsys: each rule of src/quest/combat/memsys.cpp and memsys_targets.h removed or bent once; src/quest/tests/memsys_test.cpp must fail on every one
# (heap_step_test.cpp for the ones only the shipped table sees). The `msrow_` mutants edit the install table (src/quest/combat/combat_install.cpp, a
# shared file) and are run against heap_step_test and combat_install_test: run them apart from the others (`mutants.sh memsys '^ms_'`, then
# `mutants.sh memsys '^msrow_'` while holding the shared lock). Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
#
# Not mutated, because no run can tell the mutant from the original:
#   - `!readable ||` and `stack == 0 ||` in RaisePool: an unreadable pool reads as stack 0, and a stack of 0 is refused by the mapping check that
#     follows (the Warning's `readable` field is separate, and the test asserts it);
#   - `!*readable ||` and `*have = 0;` in FirstWordMismatch: an unreadable word reads as 0, which differs from every pinned word, and the line's
#     `have` comes from the separate `readable` value;
#   - `ok &&` in the witness's `known = ok && mib != 0`: a failed read leaves 0 bytes, so `mib != 0` is false either way;
#   - `if (got != sentinel::GotStatus::kOk) Thunk::Disarm();` in InstallRow: a thunk whose install failed is on no slot, so nothing reaches its handler;
#   - `.Owned("combat_memsys")` in the two targets: a name for the chain the manager shows when an entry refuses a hook, which this module never meets;
#   - the test seams and ResetForTest.
F=src/quest/combat/memsys.cpp
T=src/quest/combat/memsys_targets.h
CI=src/quest/combat/combat_install.cpp
# the pins: slots, functions, return addresses, the System slot
for field in "0x36e9b48ULL alloc_slot" "0x1901b5cULL alloc_fn" "0x18818ccULL alloc_site" "0x36de5c0ULL init_slot" "0xf7e8e8ULL init_fn" "0xf784e8ULL init_site" "0x371d020ULL system_slot"; do
  read -r value name <<<"$field"
  hex="${value%ULL}"
  runm ms_pin_$name $T "/kPins = /s/$value/$(printf '0x%x' $((hex + 4)))ULL/"
done
# the 21 code words: each one's word changed once, and the row of the first and last
for word in "18818b8 52804001" "18818c8 97da8bfe" "18818cc a942ae6a" "18818d8 52803f6c" "18818e0 4f00c420" "18818e8 91080169" "1881904 f120011f" \
            "1881914 f9001a69" "f248c0 b0013e30" "f248c4 f945a611" "1901b5c d10143ff" "1901c24 ad0002c0" "1901c30 f9001e75" "1901c4c f9001660" \
            "f784e4 97fe5633" "f784e8 97fdb586" "f0ddb0 b0013e90" "f0ddb4 f942e211" "f78510 b0013d28" "f78514 f9401108" "f78524 f9400101"; do
  read -r va w <<<"$word"
  runm ms_word_$va $T "s/{0x$va, 0x$w, /{0x$va, 0x$(printf '%x' $((0x$w ^ 1))), /"
done
runm ms_word_row_first  $T "s/{0x18818b8, 0x52804001, 37}/{0x18818b8, 0x52804001, 38}/"
runm ms_word_row_last   $T "s/{0xf78524, 0xf9400101, 38}/{0xf78524, 0xf9400101, 37}/"
# the layout of the pool, the two capacities, the two symbols
runm ms_off_stack       $T "s/kOffStack = 0x28,/kOffStack = 0x2c,/"
runm ms_off_count       $T "s/kOffCount = 0x30,/kOffCount = 0x34,/"
runm ms_off_stack_cap   $T "s/kOffStackCap = 0x38,/kOffStackCap = 0x3c,/"
runm ms_off_cap         $T "s/kOffCap = 0x84;/kOffCap = 0x88;/"
runm ms_cap_stock       $T "s/kCapStock = 0x200,/kCapStock = 0x201,/"
runm ms_cap_new         $T "s/kCapNew = 0x400;/kCapNew = 0x401;/"
runm ms_symbol_alloc    $T "s/AllocEjPNS_10CAllocatorE\";/AllocEjPNS_10CAllocatorX\";/"
runm ms_symbol_init     $T "s/CSysJava4InitEP11android_app\";/CSysJava4InitEP11android_apq\";/"
# the two targets: the slot, the function it must hold, the kind, the build
runm ms_alloc_expected  $T "s/  target.expectedOriginal = reinterpret_cast<const void\\*>(base + pins.allocFn);/  (void)base;/"
runm ms_alloc_expected_off $T "s/base + pins.allocFn);/base + pins.allocFn + 4);/"
runm ms_init_expected   $T "s/  target.expectedOriginal = reinterpret_cast<const void\\*>(base + pins.initFn);/  (void)base;/"
runm ms_init_expected_off $T "s/base + pins.initFn);/base + pins.initFn + 4);/"
runm ms_alloc_kind      $T "s/kAllocSymbol, sentinel::RelocKind::kJumpSlot,/kAllocSymbol, sentinel::RelocKind::kGlobDat,/"
runm ms_init_kind       $T "s/kInitSymbol, sentinel::RelocKind::kJumpSlot,/kInitSymbol, sentinel::RelocKind::kGlobDat,/"
runm ms_alloc_build_id  $T "s/LibR15PatchedBuildId(), pins.allocSlot);/kLibR15StockBuildId, pins.allocSlot);/"
runm ms_init_build_id   $T "s/LibR15PatchedBuildId(), pins.initSlot);/kLibR15StockBuildId, pins.initSlot);/"
runm ms_alloc_slot      $T "s/LibR15PatchedBuildId(), pins.allocSlot);/LibR15PatchedBuildId(), pins.initSlot);/"
runm ms_init_slot       $T "s/LibR15PatchedBuildId(), pins.initSlot);/LibR15PatchedBuildId(), pins.allocSlot);/"
# the sites: the test's own, else base + the pin
runm ms_site_alloc_override $F "/^std::uintptr_t SiteAlloc/,/^}/s/o != 0 ?/o == 0 ?/"
runm ms_site_init_override  $F "/^std::uintptr_t SiteInit/,/^}/s/o != 0 ?/o == 0 ?/"
runm ms_site_alloc_base     $F "/^std::uintptr_t SiteAlloc/,/^}/s/g_base.load(std::memory_order_acquire) + //"
runm ms_site_init_base      $F "/^std::uintptr_t SiteInit/,/^}/s/g_base.load(std::memory_order_acquire) + //"
runm ms_site_alloc_offset   $F "/^std::uintptr_t SiteAlloc/,/^}/s/g_allocSiteOffset/g_initSiteOffset/"
runm ms_site_init_offset    $F "/^std::uintptr_t SiteInit/,/^}/s/g_initSiteOffset/g_allocSiteOffset/"
runm ms_read_unchecked      $F "s/^  if (!sentinel::MappingsAllow(address, sizeof(T), \\/\\*write=\\*\\/false)) return false;//"
# the pool: each doubt that refuses to write
runm ms_pool_unaligned      $F "s/ || stack % 4 != 0 || / || stack % 4 == 0 || /"
runm ms_pool_stack_cap      $F "s/ || stackCap < kCapNew || / || /"
runm ms_pool_stack_cap_le   $F "s/ || stackCap < kCapNew || / || stackCap <= kCapNew || /"
runm ms_pool_cap            $F "s/ || cap != kCapNew || / || /"
runm ms_pool_count          $F "s/ || count != 0 ||\$/ ||/"
runm ms_pool_stack_writable $F "s/^      !sentinel::MappingsAllow(stack, sizeof(std::uint32_t) \\* kCapNew, \\/\\*write=\\*\\/true) ||\$/      false ||/"
runm ms_pool_count_writable $F "s/^      !sentinel::MappingsAllow(pool + kOffCount, sizeof(std::uint64_t), \\/\\*write=\\*\\/true)) {\$/      false) {/"
runm ms_pool_stack_size     $F "s/sizeof(std::uint32_t) \\* kCapNew, \\/\\*write=\\*\\/true)/sizeof(std::uint32_t) * kCapStock, \\/*write=*\\/true)/"
runm ms_pool_warn_once      $F "0,/if (!g_poolLogged.exchange(true)) {/s//if (true) {/"
runm ms_pool_info_once      $F "/g_poolLogged.exchange(true)/{N;/kInfo/s/if (!g_poolLogged.exchange(true)) {/if (true) {/}"
runm ms_pool_warn_level     $F "s/sentinel::LogLevel::kWarn, \"combat_memsys_pool\"/sentinel::LogLevel::kInfo, \"combat_memsys_pool\"/"
runm ms_pool_info_level     $F "s/sentinel::LogLevel::kInfo, \"combat_memsys_pool\"/sentinel::LogLevel::kDebug, \"combat_memsys_pool\"/"
runm ms_pool_stack_cap_field $F "s/{\"stack_cap\", stackCap}/{\"stack_cap\", cap}/"
runm ms_pool_cap_field      $F "s/{\"cap_read\", cap}/{\"cap_read\", stackCap}/"
runm ms_pool_count_field    $F "s/{\"count\", count}/{\"count\", cap}/"
runm ms_pool_readable_field $F "s/{\"readable\", readable ? 1 : 0}/{\"readable\", readable ? 0 : 1}/"
runm ms_pool_stack_field    $F "s/{{\"result\", \"not_raised\"}, {\"stack\", sentinel::HexString(stackText, stack)}/{{\"result\", \"not_raised\"}, {\"stack\", sentinel::HexString(stackText, stackCap)}/"
runm ms_pool_slots_field    $F "s/{\"slots\", kCapNew}/{\"slots\", kCapStock}/"
runm ms_pool_pushed_field   $F "s/{\"pre_pushed\", kCapNew - kCapStock}/{\"pre_pushed\", kCapNew}/"
runm ms_pool_read_back      $F "s/{\"cap_read_back\", cap}/{\"cap_read_back\", kCapStock}/"
runm ms_pool_consequence    $F "s/the layout pool holds 1024 render-target layouts once the inline fill has run/the pool holds 1024 layouts/"
runm ms_pool_fill_value     $F "s/free\\[i\\] = kCapNew - 1 - i;/free[i] = kCapNew - 2 - i;/"
runm ms_pool_fill_short     $F "s/i < kCapNew - kCapStock; ++i) free/i < kCapNew - kCapStock - 1; ++i) free/"
runm ms_pool_pushed         $F "s/const std::uint64_t pushed = kCapNew - kCapStock;/const std::uint64_t pushed = kCapNew - kCapStock + 1;/"
runm ms_pool_count_unwritten $F "s/^  std::memcpy(reinterpret_cast<void\\*>(pool + kOffCount), \\&pushed, sizeof(pushed));/  (void)pushed;/"
runm ms_pool_returns_true   $F "/^bool RaisePool/,/^}/s/^  return true;/  return false;/"
# R37: only InitCommon's call with the stock capacity asks for more; the game's result goes back
runm ms_alloc_uncounted     $F "s/  g_allocCalls.fetch_add(1, std::memory_order_relaxed);/  (void)0;/"
runm ms_alloc_caller_kept   $F "s/^  g_lastAllocCaller.store(reinterpret_cast<std::uintptr_t>(caller), std::memory_order_relaxed);/  (void)0;/"
runm ms_alloc_any_caller    $F "s/  if (reinterpret_cast<std::uintptr_t>(caller) != SiteAlloc() || capacity != kCapStock || pool == nullptr) {/  if ((SiteAlloc() == 0 \\&\\& false) || capacity != kCapStock || pool == nullptr) {/"
runm ms_alloc_any_capacity  $F "s/  if (reinterpret_cast<std::uintptr_t>(caller) != SiteAlloc() || capacity != kCapStock || pool == nullptr) {/  if (reinterpret_cast<std::uintptr_t>(caller) != SiteAlloc() || pool == nullptr) {/"
runm ms_alloc_null_pool     $F "s/  if (reinterpret_cast<std::uintptr_t>(caller) != SiteAlloc() || capacity != kCapStock || pool == nullptr) {/  if (reinterpret_cast<std::uintptr_t>(caller) != SiteAlloc() || capacity != kCapStock) {/"
runm ms_alloc_new_cap       $F "s/  const std::uint64_t result = original(pool, kCapNew, allocator);/  const std::uint64_t result = original(pool, kCapStock, allocator);/"
runm ms_alloc_passthrough   $F "s/    return original(pool, capacity, allocator);/    return original(pool, kCapStock, allocator);/"
runm ms_alloc_result        $F "/^std::uint64_t AllocHandler/,/^}/s/^  return result;/  return result * 0;/"
runm ms_alloc_raise_inverted $F "s/  if (RaisePool(reinterpret_cast<std::uintptr_t>(pool))) {/  if (!RaisePool(reinterpret_cast<std::uintptr_t>(pool))) {/"
runm ms_alloc_raised_uncounted $F "s/    g_poolRaised.fetch_add(1, std::memory_order_relaxed);/    (void)0;/"
runm ms_alloc_not_raised_uncounted $F "s/    g_poolNotRaised.fetch_add(1, std::memory_order_relaxed);/    (void)0;/"
# R38: the witness once, at android_main's call, and every call goes on to the game's function
runm ms_witness_unit        $F "s/const std::uint64_t mib = bytes >> 20;/const std::uint64_t mib = bytes >> 21;/"
runm ms_witness_known_zero  $F "s/  const bool known = ok \\&\\& mib != 0;.*\$/  const bool known = ok;/"
runm ms_witness_read_both   $F "s/  const bool ok = ReadGame(slot, \\&word) \\&\\& ReadGame(word, \\&bytes);/  const bool ok = ReadGame(slot, \\&word) || ReadGame(word, \\&bytes);/"
runm ms_witness_slot_base   $F "s/  const std::uintptr_t slot = g_base.load(std::memory_order_acquire) + g_systemSlotOffset.load(std::memory_order_acquire);/  const std::uintptr_t slot = g_systemSlotOffset.load(std::memory_order_acquire);/"
runm ms_witness_level       $F "s/sentinel::LogLevel::kInfo, \"combat_memsys_witness\"/sentinel::LogLevel::kDebug, \"combat_memsys_witness\"/"
runm ms_witness_msg         $F "s/\"android_main reached CSysJava::Init\"/\"android_main reached Init\"/"
runm ms_witness_value_field $F "s/known ? sentinel::LogField{\"system_heap_mib_next_read\", mib}/known ? sentinel::LogField{\"system_heap_mib_next_read\", bytes}/"
runm ms_witness_unknown     $F "s/: sentinel::LogField{\"system_heap_mib_next_read\", \"unreadable\"}/: sentinel::LogField{\"system_heap_mib_next_read\", mib}/"
runm ms_witness_class       $F "s/{\"system_heap\", !known ? \"unreadable\" : (mib == kStockSystemMiB ? \"stock\" : \"raised\")}/{\"system_heap\", !known ? \"unreadable\" : (mib == kStockSystemMiB ? \"raised\" : \"stock\")}/"
runm ms_witness_unknown_class $F "s/{\"system_heap\", !known ? \"unreadable\" :/{\"system_heap\", known ? \"unreadable\" :/"
runm ms_witness_stock_size  $F "s/kStockSystemMiB = 400;/kStockSystemMiB = 401;/"
runm ms_witness_slot_field  $F "s/{\"system_slot_vaddr\", sentinel::HexString(slotText, g_systemSlotOffset.load(std::memory_order_acquire))}/{\"system_slot_vaddr\", sentinel::HexString(slotText, 0)}/"
runm ms_init_uncounted      $F "s/  g_initCalls.fetch_add(1, std::memory_order_relaxed);/  (void)0;/"
runm ms_init_caller_kept    $F "s/^  g_lastInitCaller.store(reinterpret_cast<std::uintptr_t>(caller), std::memory_order_relaxed);/  (void)0;/"
runm ms_init_any_caller     $F "s/  if (reinterpret_cast<std::uintptr_t>(caller) == SiteInit() \\&\\& !g_initSeen.exchange(true)) Witness();/  if ((SiteInit() == 0 || true) \\&\\& !g_initSeen.exchange(true)) Witness();/"
runm ms_init_other_caller   $F "s/  if (reinterpret_cast<std::uintptr_t>(caller) == SiteInit() \\&\\& !g_initSeen.exchange(true)) Witness();/  if (reinterpret_cast<std::uintptr_t>(caller) != SiteInit() \\&\\& !g_initSeen.exchange(true)) Witness();/"
runm ms_init_every_call     $F "s/  if (reinterpret_cast<std::uintptr_t>(caller) == SiteInit() \\&\\& !g_initSeen.exchange(true)) Witness();/  if (reinterpret_cast<std::uintptr_t>(caller) == SiteInit()) Witness();/"
runm ms_init_no_witness     $F "s/\\&\\& !g_initSeen.exchange(true)) Witness();/\\&\\& !g_initSeen.exchange(true)) { if (false) Witness(); }/"
runm ms_init_arg            $F "s/  return original(app);/  (void)app;\\n  return original(nullptr);/"
runm ms_init_result         $F "s/  return original(app);/  return original(app) * 0;/"
# a pinned word that differs refuses its row alone
runm ms_words_other_row     $F "s/    if (pin.row != row) continue;/    if (pin.row == row) continue;/"
runm ms_words_cmp           $F "s/    if (!\\*readable || \\*have != pin.word) return \\&pin;/    if (!*readable || *have == pin.word) return \\&pin;/"
runm ms_mismatch_level      $F "s/sentinel::LogFields(sentinel::LogLevel::kWarn, \"combat_memsys_hook\",/sentinel::LogFields(sentinel::LogLevel::kError, \"combat_memsys_hook\",/"
runm ms_mismatch_symbol     $F "s/{\"symbol\", symbol}, {\"status\", \"pinned_word_mismatch\"}/{\"symbol\", row}, {\"status\", \"pinned_word_mismatch\"}/"
runm ms_mismatch_vaddr      $F "s/{\"vaddr\", sentinel::HexString(at, mismatch->vaddr)}/{\"vaddr\", sentinel::HexString(at, mismatch->word)}/"
runm ms_mismatch_readable   $F "s/{\"have\", readable ? sentinel::HexString(haveText, have) : \"unreadable\"}/{\"have\", sentinel::HexString(haveText, have)}/"
runm ms_mismatch_want       $F "s/{\"want\", sentinel::HexString(wantText, mismatch->word)}/{\"want\", sentinel::HexString(wantText, have)}/"
runm ms_mismatch_consequence $F "s/the row is not installed: its pinned code is not the pinned build's/the row is not installed/"
runm ms_mismatch_token      $F "s/    return \"pinned_word_mismatch\";/    return \"mismatch\";/"
runm ms_row_not_armed       $F "s/^  Thunk::Arm(record);/  (void)record;/"
runm ms_row_level           $F "s/got == sentinel::GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_memsys_hook\"/got == sentinel::GotStatus::kOk ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, \"combat_memsys_hook\"/"
runm ms_row_symbol          $F "s/{\"symbol\", symbol}, {\"status\", status}}/{\"symbol\", row}, {\"status\", status}}/"
runm ms_row_status          $F "s/  status = sentinel::GotStatusName(got);/  status = \"ok\";/"
# the step: off, once, the image, the build ID, what the rows get
runm ms_off_inverted        $F "s/^  if (!on) {/  if (on) {/"
runm ms_off_flag            $F "s/    return {false, \"off\", true};/    return {false, \"off\", false};/" quest-host-combat heap_step_test
runm ms_off_text            $F "s/{{\"result\", \"off\"}, {\"reason\", \"combat.memsys\"}/{{\"result\", \"off\"}, {\"reason\", \"combat.memsy\"}/"
runm ms_off_consequence     $F "s/combat.memsys is false: the layout pool is the stock 512 slots and the System heap is the game's/combat.memsys is false/"
runm ms_ran_not_set         $F "s/g_ran.exchange(true, std::memory_order_acq_rel)/g_ran.load(std::memory_order_acquire)/"
runm ms_ran_level           $F "s/sentinel::LogLevel::kWarn, \"combat_memsys_install\"/sentinel::LogLevel::kInfo, \"combat_memsys_install\"/"
runm ms_ran_status          $F "s/    return {false, \"already_ran\"};/    return {false, \"off\"};/"
runm ms_ran_text            $F "s/{\"reason\", \"already_ran\"}, {\"consequence\", \"a second step installs nothing\"}/{\"reason\", \"already\"}, {\"consequence\", \"a second step installs nothing\"}/"
runm ms_lookup_null         $F "s/  if (lookup == nullptr || !lookup(pinned::kLibR15, \\&image)) return/  if (!lookup(pinned::kLibR15, \\&image)) return/"
runm ms_lookup_inverted     $F "s/  if (lookup == nullptr || !lookup(pinned::kLibR15, \\&image)) return/  if (lookup == nullptr || lookup(pinned::kLibR15, \\&image)) return/"
runm ms_build_id_unread     $F "s/^  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\");//"
runm ms_build_id_cmp        $F "s/std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0/std::strcmp(id, pinned::LibR15PatchedBuildId()) == 0/"
runm ms_build_id_stock      $F "s/std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0/std::strcmp(id, pinned::kLibR15StockBuildId) != 0/"
runm ms_refused_token       $F "s/  return {false, token};/  return {false, \"refused\"};/"
runm ms_refused_level       $F "s/^  sentinel::LogFields(sentinel::LogLevel::kError, \"combat_memsys_install\",\$/  sentinel::LogFields(sentinel::LogLevel::kWarn, \"combat_memsys_install\",/"
runm ms_refused_rows        $F "s/{\"r37\", \"not_tried\"}, {\"r38\", \"not_tried\"}/{\"r37\", \"not_tried\"}, {\"r38\", \"tried\"}/"
runm ms_refused_consequence $F "s/the layout pool is the stock 512 slots and no witness line is written\"}});\$/the pool is the stock 512 slots\"}});/"
runm ms_pins_alloc_site     $F "s/  g_allocSiteOffset.store(pins.allocSite, std::memory_order_release);/  (void)pins;/"
runm ms_pins_init_site      $F "s/  g_initSiteOffset.store(pins.initSite, std::memory_order_release);/  (void)pins;/"
runm ms_pins_system_slot    $F "s/  g_systemSlotOffset.store(pins.systemSlot, std::memory_order_release);/  (void)pins;/"
runm ms_pins_base           $F "s/  g_base.store(image.base, std::memory_order_release);.*\$/  (void)image;/"
runm ms_row_alloc_number    $F "s/InstallRow<AllocThunk>(\"R37\", kAllocSymbol, 37,/InstallRow<AllocThunk>(\"R37\", kAllocSymbol, 38,/"
runm ms_row_init_number     $F "s/InstallRow<InitThunk>(\"R38\", kInitSymbol, 38,/InstallRow<InitThunk>(\"R38\", kInitSymbol, 37,/"
runm ms_row_alloc_name      $F "s/InstallRow<AllocThunk>(\"R37\",/InstallRow<AllocThunk>(\"R38\",/"
runm ms_row_alloc_pins      $F "s/AllocTarget(image.base, pins),/AllocTarget(image.base, kPins),/"
runm ms_row_init_pins       $F "s/InitTarget(image.base, pins),/InitTarget(image.base, kPins),/"
runm ms_ok37_cmp            $F "s/const bool ok37 = std::strcmp(r37, \"ok\") == 0;/const bool ok37 = std::strcmp(r37, \"ok\") != 0;/"
runm ms_ok38_cmp            $F "s/const bool ok38 = std::strcmp(r38, \"ok\") == 0;/const bool ok38 = std::strcmp(r38, \"ok\") != 0;/"
runm ms_result_installed    $F "s/  const char\\* const result = ok37 \\&\\& ok38 ? \"installed\" :/  const char* const result = ok37 || ok38 ? \"installed\" :/"
runm ms_result_partial      $F "s/(ok37 || ok38 ? \"partial\" : \"refused\");/(ok37 \\&\\& ok38 ? \"partial\" : \"refused\");/"
runm ms_install_level       $F "s/sentinel::LogFields(ok37 \\&\\& ok38 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_memsys_install\"/sentinel::LogFields(ok37 || ok38 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_memsys_install\"/"
runm ms_install_reason      $F "s/{\"reason\", ok37 \\&\\& ok38 ? \"ok\" : \"row_refused\"}/{\"reason\", ok37 \\&\\& ok38 ? \"row_refused\" : \"ok\"}/"
runm ms_install_rows        $F "s/{\"r37\", r37}, {\"r38\", r38},/{\"r37\", r38}, {\"r38\", r37},/"
runm ms_consequence_pool    $F "s/: (ok37 ? \"the pool is raised but android_main's start is not witnessed\"/: (ok38 ? \"the pool is raised but android_main's start is not witnessed\"/"
runm ms_consequence_witness $F "s/(ok38 ? \"the witness is written but the pool is the stock 512 slots\"/(ok37 ? \"the witness is written but the pool is the stock 512 slots\"/"
runm ms_consequence_ok      $F "s/the layout pool is raised to 1024 slots and android_main's start is witnessed/the pool is raised/"
runm ms_installed_flag      $F "s/  return {ok37 \\&\\& ok38, result};/  return {ok37 || ok38, result};/"
# the entry the sentinel calls: the seams for the lookup and the pins
runm ms_entry_lookup        $F "s/InstallAt(lookup != nullptr ? lookup : \\&sentinel::FindLoadedImage, pins != nullptr ? \\*pins : kPins, on);/InstallAt(lookup != nullptr ? \\&sentinel::FindLoadedImage : \\&sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, on);/" quest-host-combat heap_step_test
runm ms_entry_pins          $F "s/pins != nullptr ? \\*pins : kPins, on);/pins != nullptr ? kPins : kPins, on);/" quest-host-combat heap_step_test
# the counters
runm ms_stats_witnessed     $F "s/g_initSeen.load() ? 1U : 0U};/g_initSeen.load() ? 0U : 1U};/"
runm ms_stats_order         $F "s/  return {g_allocCalls.load(), g_poolRaised.load(), g_poolNotRaised.load(), g_initCalls.load(),/  return {g_allocCalls.load(), g_poolNotRaised.load(), g_poolRaised.load(), g_initCalls.load(),/"
# the table row (combat_install.cpp, shared) and its tests: the switch reaches the module, the row is early, it follows heaps
runm msrow_key              $CI "s/{ return memsys::Install(g_config.memsys.on); }/{ return memsys::Install(true); }/" quest-host-combat heap_step_test
runm msrow_early_slot       $CI "s/Module{\"memsys\", \\&MemsysEarly, nullptr}/Module{\"memsys\", nullptr, \\&MemsysEarly}/" quest-host-combat combat_install_test
runm msrow_name             $CI "s/Module{\"memsys\", \\&MemsysEarly, nullptr}/Module{\"memsyss\", \\&MemsysEarly, nullptr}/" quest-host-combat combat_install_test
runm msrow_order            $CI "/Module{\"heaps\", /{s/heaps/memsys/;s/HeapsEarly/MemsysEarly/;p;d};/^    Module{\"memsys\", /{s/memsys/heaps/;s/MemsysEarly/HeapsEarly/}" quest-host-combat combat_install_test
