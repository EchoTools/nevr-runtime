# combat_tick: each rule of src/quest/combat/combat_tick.cpp, combat_tick.h and combat_tick_targets.h removed or bent once; src/quest/tests/combat_tick_test.cpp
# must fail on every one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target>]`.
# Run: src/quest/combat/mutants.sh combat_tick
#
# Not listed, because no host test can tell them from the original (named here so the list is read as complete):
#   - `fn != nullptr` in RunCallbacks: a row the count already covers but whose store has not happened yet is reachable only from a second thread
#     between Add's compare-exchange and its store; one thread cannot make a frame run in that window.
#   - `i < kMaxCallbacks` in RunCallbacks: Add never lets the count pass kMaxCallbacks, so the second term never decides.
#   - `!readable ||` in OnTick: an unreadable object leaves vptr 0, which is not the pinned value either, so the vptr compare refuses it alone.
#   - the `lookup` argument of InstallNormal, and the `ok` branch of its result (`{ok, ok ? "installed" : ..}`): on the host `sentinel::FindLoadedImage`
#     finds no libr15, so the step can only refuse; its installed answer is the arm64 and headset check's.
#   - the `if (status == kOk)` test in InstallStatusAt: a lookup that fails gives the same refusal from the manager.
F=src/quest/combat/combat_tick.cpp
H=src/quest/combat/combat_tick.h
T=src/quest/combat/combat_tick_targets.h
# the callback table: 16 rows, a null refused, registration order
runm ct_max_17              $H "s/kMaxCallbacks = 16;/kMaxCallbacks = 17;/"
runm ct_max_15              $H "s/kMaxCallbacks = 16;/kMaxCallbacks = 15;/"
runm ct_add_null_ok         $F "s/  if (fn == nullptr) return false;//"
runm ct_add_cap_off_by_one  $F "s/    if (index >= kMaxCallbacks) return false;/    if (index > kMaxCallbacks) return false;/"
runm ct_add_cap_early       $F "s/    if (index >= kMaxCallbacks) return false;/    if (index + 1 >= kMaxCallbacks) return false;/"
runm ct_add_cap_removed     $F "s/    if (index >= kMaxCallbacks) return false;//"
runm ct_add_row_zero        $F "s/  g_ticks\[index\].store(fn, std::memory_order_release);/  g_ticks[0].store(fn, std::memory_order_release);/"
runm ct_add_count_skips     $F "s/g_count.compare_exchange_weak(index, index + 1, /g_count.compare_exchange_weak(index, index + 2, /"
runm ct_add_returns_false   $F "s/^  return true;\$/  return false;/"
runm ct_count_zero          $F "s/std::size_t Count() noexcept { return g_count.load(std::memory_order_acquire); }/std::size_t Count() noexcept { return 0; }/"
# the walk: every registered row, in order, with the verified object
runm ct_walk_reversed       $F "s/    const TickFn fn = g_ticks\[i\].load(std::memory_order_acquire);/    const TickFn fn = g_ticks[n - 1 - i].load(std::memory_order_acquire);/"
runm ct_walk_skips_first    $F "s/  for (std::size_t i = 0; i < n \&\& i < kMaxCallbacks; ++i) {/  for (std::size_t i = 1; i < n \&\& i < kMaxCallbacks; ++i) {/"
runm ct_walk_skips_last     $F "s/  for (std::size_t i = 0; i < n \&\& i < kMaxCallbacks; ++i) {/  for (std::size_t i = 0; i + 1 < n \&\& i < kMaxCallbacks; ++i) {/"
runm ct_walk_object_zero    $F "s/    if (fn != nullptr) fn(object);/    if (fn != nullptr) fn(object \& 0);/"
# the handler: the game's function first, with the game, once per frame
runm ct_original_missing    $F "s/^  original(game);\$/  (void)original;/"
runm ct_original_arg        $F "s/^  original(game);\$/  original(nullptr);/"
runm ct_original_twice      $F "s/^  original(game);\$/  original(game); original(game);/"
runm ct_original_last       $F "s/^  original(game);\$/  (void)original;/;s/^\(  *\)RunCallbacks(object);\$/\1RunCallbacks(object); original(game);/"
runm ct_calls_uncounted     $F "s/g_calls.fetch_add(1, std::memory_order_relaxed);/g_calls.fetch_add(0, std::memory_order_relaxed);/"
runm ct_calls_by_two        $F "s/g_calls.fetch_add(1, std::memory_order_relaxed);/g_calls.fetch_add(2, std::memory_order_relaxed);/"
runm ct_calls_reads_zero    $F "s/std::uint32_t Calls() noexcept { return g_calls.load(std::memory_order_relaxed); }/std::uint32_t Calls() noexcept { return 0; }/"
# the vptr check, once per object: the verified object takes the fast path
runm ct_fast_no_callbacks   $F "s/^    RunCallbacks(object);\$/    (void)object;/"
runm ct_fast_no_return      $F "/^    RunCallbacks(object);\$/{n;s/^    return;\$/    (void)0;/}"
runm ct_slow_no_callbacks   $F "s/^  RunCallbacks(object);\$/  (void)object;/"
runm ct_null_object_fast    $F "s/  if (object != 0 \&\& object == g_verified/  if (object == g_verified/"
runm ct_verified_inverted   $F "s/object == g_verified.load(std::memory_order_relaxed) \&\&/object != g_verified.load(std::memory_order_relaxed) \&\&/"
runm ct_verified_not_set    $F "s/^  g_verified.store(object, std::memory_order_relaxed);\$/  (void)object;/"
runm ct_verified_kept       $F "/^void OnTick/,/^}/s/^  g_verified.store(0, std::memory_order_relaxed);\$/  (void)0;/"
runm ct_vptr_reads_uncounted $F "s/g_vptrReads.fetch_add(1, std::memory_order_relaxed);/g_vptrReads.fetch_add(0, std::memory_order_relaxed);/"
runm ct_vptr_reads_zero     $F "s/std::uint32_t VptrReads() noexcept { return g_vptrReads.load(std::memory_order_relaxed); }/std::uint32_t VptrReads() noexcept { return 0; }/"
# ... and again every 256 frames, so a reused address cannot keep running callbacks for ever
runm ct_recheck_257         $F "s/constexpr std::uint32_t kRecheckFrames = 256;/constexpr std::uint32_t kRecheckFrames = 257;/"
runm ct_recheck_255         $F "s/constexpr std::uint32_t kRecheckFrames = 256;/constexpr std::uint32_t kRecheckFrames = 255;/"
runm ct_recheck_le          $F "s/g_sinceCheck.fetch_add(1, std::memory_order_relaxed) + 1 < kRecheckFrames) {/g_sinceCheck.fetch_add(1, std::memory_order_relaxed) + 1 <= kRecheckFrames) {/"
runm ct_recheck_early       $F "s/g_sinceCheck.fetch_add(1, std::memory_order_relaxed) + 1 < kRecheckFrames) {/g_sinceCheck.fetch_add(1, std::memory_order_relaxed) < kRecheckFrames) {/"
runm ct_recheck_by_two      $F "s/g_sinceCheck.fetch_add(1, std::memory_order_relaxed) + 1 < kRecheckFrames) {/g_sinceCheck.fetch_add(2, std::memory_order_relaxed) + 1 < kRecheckFrames) {/"
runm ct_recheck_never       $F "s/g_sinceCheck.fetch_add(1, std::memory_order_relaxed) + 1 < kRecheckFrames) {/g_sinceCheck.fetch_add(1, std::memory_order_relaxed) + 1 < 0xffffffffu) {/"
runm ct_since_not_reset     $F "/^void OnTick/,/^}/s/^  g_sinceCheck.store(0, std::memory_order_relaxed);\$/  (void)0;/"
# an object that is not the CR15Game: no callback, one loud Error, the game's frame still ran
runm ct_vptr_unchecked      $F "s/  if (!readable || vptr != g_vptr.load(std::memory_order_relaxed)) {/  if (!readable) {/"
runm ct_vptr_equal          $F "s/  if (!readable || vptr != g_vptr.load(std::memory_order_relaxed)) {/  if (!readable || vptr == g_vptr.load(std::memory_order_relaxed)) {/"
runm ct_readable_inverted   $F "s/  if (!readable || vptr != g_vptr.load(std::memory_order_relaxed)) {/  if (readable || vptr != g_vptr.load(std::memory_order_relaxed)) {/"
runm ct_miss_falls_through  $F "/^    }\$/{n;s/^    return;\$/    (void)0;/}"
runm ct_miss_every_time     $F "s/g_objectMisses.fetch_add(1, std::memory_order_relaxed) == 0) {/g_objectMisses.fetch_add(1, std::memory_order_relaxed) < 1000000u) {/"
runm ct_miss_twice          $F "s/g_objectMisses.fetch_add(1, std::memory_order_relaxed) == 0) {/g_objectMisses.fetch_add(1, std::memory_order_relaxed) < 2) {/"
runm ct_miss_never          $F "s/g_objectMisses.fetch_add(1, std::memory_order_relaxed) == 0) {/g_objectMisses.fetch_add(1, std::memory_order_relaxed) == 1) {/"
runm ct_miss_level          $F "s/sentinel::LogLevel::kError, \"combat_tick_object\"/sentinel::LogLevel::kWarn, \"combat_tick_object\"/"
runm ct_miss_readable_field $F "s/{{\"readable\", readable ? 1 : 0},/{{\"readable\", 1},/"
runm ct_miss_msg            $F "s/the object the game's frame update was called with is not the CR15Game; the combat callbacks \"/the object the game's frame update was called with is the CR15Game; the combat callbacks \"/"
# the verified game, for a module that reads it outside a frame: never cleared by a refused frame
runm ct_game_not_set        $F "s/^  g_game.store(object, std::memory_order_release);\$/  (void)object;/"
runm ct_game_cleared_on_miss $F "s/^  if (!readable || vptr != g_vptr.load(std::memory_order_relaxed)) {\$/&\n    g_game.store(0, std::memory_order_relaxed);/"
runm ct_game_reads_zero     $F "s/std::uintptr_t Game() noexcept { return g_game.load(std::memory_order_acquire); }/std::uintptr_t Game() noexcept { return 0; }/"
# the install: the vptr this process holds, the arm, the slot, the line
runm ct_install_not_armed   $F "s/^  Thunk::Arm(kTickHook);\$/  (void)kTickHook;/"
runm ct_install_null_lookup $F "s/lookup != nullptr \&\& lookup(sentinel::pinned::kLibR15, \&image)/lookup(sentinel::pinned::kLibR15, \&image)/"
runm ct_install_lookup_failed $F "s/: sentinel::GotStatus::kModuleNotLoaded;/: sentinel::GotStatus::kSymbolNotFound;/"
runm ct_install_vptr_base   $F "s/g_vptr.store(image.base + targets::kGameVptrVaddr, /g_vptr.store(targets::kGameVptrVaddr, /"
runm ct_install_vptr_value  $F "s/g_vptr.store(image.base + targets::kGameVptrVaddr, /g_vptr.store(image.base + targets::kStockVaddr, /"
runm ct_install_flag_true   $F "s/g_installed.store(ok, std::memory_order_release);/g_installed.store(true, std::memory_order_release);/"
runm ct_install_flag_false  $F "s/g_installed.store(ok, std::memory_order_release);/g_installed.store(false, std::memory_order_release);/"
runm ct_installed_reads_true $F "s/bool Installed() noexcept { return g_installed.load(std::memory_order_acquire); }/bool Installed() noexcept { return true; }/"
runm ct_line_level          $F "s/sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_tick_install\",/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_tick_install\",/"
runm ct_line_level_swapped  $F "s/sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_tick_install\",/sentinel::LogFields(ok ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, \"combat_tick_install\",/"
runm ct_line_result         $F "s/{{\"result\", ok ? \"installed\" : \"refused\"},/{{\"result\", ok ? \"refused\" : \"installed\"},/"
runm ct_line_status         $F "s/{\"status\", sentinel::GotStatusName(status)},/{\"status\", \"ok\"},/"
runm ct_line_callbacks      $F "s/{\"callbacks\", Count()},/{\"callbacks\", 0},/"
runm ct_line_msg_swapped    $F "s/{\"msg\", ok ? \"the game-thread tick is installed;/{\"msg\", !ok ? \"the game-thread tick is installed;/"
runm ct_normal_ok_true      $F "s/  return {ok, ok ? \"installed\" : sentinel::GotStatusName(status)};/  return {true, ok ? \"installed\" : sentinel::GotStatusName(status)};/"
runm ct_normal_status_word  $F "s/  return {ok, ok ? \"installed\" : sentinel::GotStatusName(status)};/  return {ok, \"installed\"};/"
runm ct_at_status_ignored   $F "s/bool InstallAt(sentinel::ImageLookup lookup) noexcept { return InstallStatusAt(lookup) == sentinel::GotStatus::kOk; }/bool InstallAt(sentinel::ImageLookup lookup) noexcept { return InstallStatusAt(lookup) != sentinel::GotStatus::kOk; }/"
# the pinned slot and the function it holds
runm ct_target_symbol       $T "s/_ZN10NRadEngine8CncaGame19ExclusiveUpdateGameEv/_ZN10NRadEngine8CncaGame19ExclusiveUpdateGamesEv/"
runm ct_target_slot         $T "s/kSlotVaddr = 0x36e7f28ULL;/kSlotVaddr = 0x36e7f20ULL;/"
runm ct_target_stock        $T "s/kStockVaddr = 0x17ef62cULL;/kStockVaddr = 0x17ef628ULL;/"
runm ct_target_vptr         $T "s/kGameVptrVaddr = 0x362ed30ULL;/kGameVptrVaddr = 0x362ed38ULL;/"
runm ct_target_kind         $T "s/sentinel::RelocKind::kJumpSlot,/sentinel::RelocKind::kGlobDat,/"
runm ct_target_build_id     $T "s/sentinel::pinned::LibR15PatchedBuildId(), kSlotVaddr}/sentinel::pinned::kLibR15StockBuildId, kSlotVaddr}/"
runm ct_target_owner        $T "s/.Owned(\"combat_tick\");/.Owned(\"combat_ticks\");/"
runm ct_target_module       $T "s/GotTarget{sentinel::pinned::kLibR15, kSymbol,/GotTarget{\"libpnsovr.so\", kSymbol,/"
