# voipdrop: each rule of src/quest/combat/voipdrop.cpp and voipdrop_targets.h removed or bent once; src/quest/tests/voipdrop_test.cpp must fail on every
# one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target>]`.
F=src/quest/combat/voipdrop.cpp
T=src/quest/combat/voipdrop_targets.h
H=src/quest/combat/voipdrop.h
# the decision: a head of -1 means the next packet needs a page; a page that cannot be had drops the packet
runm vd_head_cmp        $F "s/  if (head == -1) {  \/\/ the next packet needs a fresh page/  if (head != -1) {  \/\/ the next packet needs a fresh page/"
runm vd_head_any        $F "s/  if (head == -1) {  \/\/ the next packet needs a fresh page/  if (head == -1 || head != -1) {  \/\/ the next packet needs a fresh page/"
runm vd_never_drops     $F "s/      if (!PageAvailable(cs, page)) {/      if (page == 0xdeadbeef \&\& !PageAvailable(cs, page)) {/"
runm vd_always_drops    $F "s/      if (!PageAvailable(cs, page)) {/      if (!PageAvailable(cs, page) || true) {/"
runm vd_drop_inverted   $F "s/      if (!PageAvailable(cs, page)) {/      if (PageAvailable(cs, page)) {/"
runm vd_unread_page     $F "s/    if (ok) {\$/    if (true) {/"
runm vd_drop_calls_real $F "s/        LogDrop(page, g_dropped.fetch_add(1, std::memory_order_relaxed) + 1);/        LogDrop(page, g_dropped.fetch_add(1, std::memory_order_relaxed) + 1);\n        original(self, handle, event, data, bytes);/"
# the receiver's layout and the allocator's vtable
runm vd_free_slot       $T "s/kVtFree = 0x38/kVtFree = 0x30/"
# the probe: given back, the context switched and restored, doubt runs the game's function
runm vd_no_give_back    $F "s/      if (page != nullptr) reinterpret_cast<FreeFn>(release)(allocator, page);/      (void)release;/"
runm vd_no_restore      $F "s/  g_f.setCurrent(was);/  (void)was;/"
runm vd_no_switch       $F "s/  g_f.setCurrent(g_f.blockAllocator(reinterpret_cast<const void\*>(cs + kPageBlock)));/  (void)g_f.blockAllocator(reinterpret_cast<const void*>(cs + kPageBlock));/"
runm vd_null_alloc_ok   $F "s/  bool available = false;/  bool available = true;/"
runm vd_doubt_drops     $F "s/      available = true;/      available = false;/"
runm vd_no_release_chk  $F "s/    if (!ok || request == 0 || release == 0) {/    if (!ok || request == 0) {/"
runm vd_size_zero       $F "s/reinterpret_cast<AllocFn>(request)(allocator, bytes)/reinterpret_cast<AllocFn>(request)(allocator, bytes + 8)/"
# the arguments reach the game's function unchanged
runm vd_arg_event       $F "s/  original(self, handle, event, data, bytes);/  original(self, handle, data, event, bytes);/"
runm vd_arg_bytes       $F "s/  original(self, handle, event, data, bytes);/  original(self, handle, event, data, bytes + 1);/"
runm vd_arg_handle      $F "s/  original(self, handle, event, data, bytes);/  original(self, handle + 1, event, data, bytes);/"
runm vd_arg_self        $F "s/  original(self, handle, event, data, bytes);/  original(nullptr, handle, event, data, bytes);/"
# the counters and the lines
runm vd_packets_uncounted $F "s/g_packets.fetch_add(1, std::memory_order_relaxed) + 1;/g_packets.fetch_add(0, std::memory_order_relaxed) + 1;/"
runm vd_probed_uncounted  $F "s/      g_probed.fetch_add(1, std::memory_order_relaxed);/      g_probed.fetch_add(0, std::memory_order_relaxed);/"
runm vd_not_counted     $F "s/g_dropped.fetch_add(1, std::memory_order_relaxed) + 1)/g_dropped.fetch_add(0, std::memory_order_relaxed) + 1)/"
runm vd_lines_cap       $H "s/kDropLines = 16;/kDropLines = 17;/"
runm vd_total_every     $H "s/kTotalEvery = 50;/kTotalEvery = 49;/"
runm vd_total_cap       $H "s/kTotalLines = 16;/kTotalLines = 17;/"
runm vd_dropped_level   $F "s/sentinel::LogLevel::kInfo, \"combat_voipdrop_dropped\"/sentinel::LogLevel::kDebug, \"combat_voipdrop_dropped\"/"
runm vd_total_level     $F "s/sentinel::LogLevel::kInfo, \"combat_voipdrop_dropped_total\"/sentinel::LogLevel::kDebug, \"combat_voipdrop_dropped_total\"/"
runm vd_page_bytes_zero $F "s/{{\"n\", n}, {\"page_bytes\", pageBytes},/{{\"n\", n}, {\"page_bytes\", pageBytes + 1},/"
runm vd_stats_never     $F "s/  if ((seen \& (kStatsEvery - 1)) == 0) LogStats(false);/  (void)seen;/"
runm vd_stats_every       $F "s/  if ((seen \& (kStatsEvery - 1)) == 0) LogStats(false);/  if ((seen \& (kStatsEvery - 1)) == 0 || true) LogStats(false);/"
runm vd_stats_cadence   $H "s/kStatsEvery = 16384;/kStatsEvery = 8192;/"
runm vd_stats_final_never  $F "s/  if (g_hook.installed()) LogStats(true);/  (void)g_hook;/"
runm vd_stats_final_always $F "s/  if (g_hook.installed()) LogStats(true);/  LogStats(true);/"
runm vd_stats_final_debug  $F "s/final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, \"combat_voipdrop_stats\"/sentinel::LogLevel::kDebug, \"combat_voipdrop_stats\"/"
runm vd_stats_dropped      $F "s/{\"dropped\", g_dropped.load(std::memory_order_relaxed)},/{\"dropped\", 0},/"
# the install: the slot must hold ReceiveVoip itself, the pins gate it, the exports gate it, the build ID gates it
runm vd_no_expected     $F "s/  target.expectedOriginal = reinterpret_cast<const void\*>(image.base + pins.fn);/  (void)pins.fn;/"
runm vd_expected_wrong  $F "s/image.base + pins.fn);/image.base + pins.fn + 4);/"
runm vd_pins_skipped        $F "s/  if (pinned::FirstFailure(image.base, \&kWordPins, 1, \&word)) return Refused(\"pinned_word_mismatch\", \&word);/  if (false) return Refused(\"pinned_word_mismatch\", \&word);/"
runm vd_last_pin_skipped    $T "s/kWordPins = {kWords, kWordCount, nullptr, 0};/kWordPins = {kWords, kWordCount - 1, nullptr, 0};/"
runm vd_pin_unreadable_ok $F "s/word->readable ? sentinel::HexString(haveText, word->have) : \"unreadable\"/sentinel::HexString(haveText, word->have)/"
runm vd_build_id_skipped    $F "s/  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\");//"
runm vd_no_build_id_ok  $F "s/  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused(\"no_build_id\");//"
runm vd_no_fn_check     $F "s/  if (fns.blockAllocator == nullptr || fns.currentPtr == nullptr || fns.setCurrent == nullptr || fns.current == nullptr) return Refused(\"export_missing\");//"
runm vd_fn_check_first  $F "s/fns.blockAllocator == nullptr || fns.currentPtr == nullptr ||/fns.currentPtr == nullptr ||/"
runm vd_fn_check_second $F "s/fns.currentPtr == nullptr || fns.setCurrent == nullptr/fns.setCurrent == nullptr/"
runm vd_fn_check_third  $F "s/fns.setCurrent == nullptr || fns.current == nullptr/fns.current == nullptr/"
runm vd_fn_check_last   $F "s/ || fns.current == nullptr) return Refused/) return Refused/"
runm vd_fns_not_kept    $F "s/  g_f = fns;  \/\/ before the slot changes.*/  (void)fns;/"
runm vd_refusal_quiet   $F "s/sentinel::LogFields(sentinel::LogLevel::kError, \"combat_voipdrop_install\",/sentinel::LogFields(sentinel::LogLevel::kDebug, \"combat_voipdrop_install\",/"
runm vd_install_level   $F "s/sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_voipdrop_install\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_voipdrop_install\"/"
runm vd_resolve_one_wrong $F "s/find(context, kCurrentSymbol)/find(context, kCurrentPtrSymbol)/"
# the switch and the legacy file
runm vd_off_ignored     $F "s/  if (!on) {/  if (!on \&\& false) {/"
runm vd_off_not_off     $F "s/    return {false, \"off_config\", true};/    return {false, \"off_config\", false};/"
runm vd_legacy_not_told $F "s/  if (legacy::Present(\"voipdrop.off\")) {/  if (false \&\& legacy::Present(\"voipdrop.off\")) {/"
runm vd_legacy_file     $F "s/legacy::Present(\"voipdrop.off\")/legacy::Present(\"voipdrop.on\")/"
runm vd_legacy_turns_off $F "s/  if (!on) {/  if (!on || legacy::Present(\"voipdrop.off\")) {/"
runm vd_legacy_quiet    $F "s/sentinel::LogLevel::kWarn, \"combat_voipdrop_legacy_switch\"/sentinel::LogLevel::kInfo, \"combat_voipdrop_legacy_switch\"/"
runm vd_legacy_no_key   $F "s/{\"key\", \"combat.voipdrop\"}, //"
# the pinned constants
runm vd_slot_const      $T "s/kReceiveVoipSlotVaddr = 0x36c5d80ULL/kReceiveVoipSlotVaddr = 0x36c5d88ULL/"
runm vd_real_const      $T "s/kReceiveVoipVaddr = 0x21c3238ULL/kReceiveVoipVaddr = 0x21c3230ULL/"
runm vd_word_changed    $T "s/0x2316504, 0xb9406661}/0x2316504, 0xb9406660}/"
# The table row (combat_install.cpp) and the key (quest_config.cpp, nevr_config.cpp) are shared files, run against combat_install_test and quest_config_test.
CI=src/quest/combat/combat_install.cpp
runm vd_row_ignores_switch $CI "s/voipdrop::Install(g_config.voipdrop.on)/voipdrop::Install(true)/" quest-host-combat combat_install_test
runm vd_row_end_null       $CI "s/voipdrop, \&voipdrop::NoteEnd}/voipdrop, nullptr}/" quest-host-combat combat_install_test
runm vd_row_early          $CI "s/Module{\"voipdrop\", nullptr, \&VoipdropNormal,/Module{\"voipdrop\", \&VoipdropNormal, nullptr,/" quest-host-combat combat_install_test
QC=src/quest/sentinel/quest_config.cpp
runm vd_qc_default_off     $QC "s/  r.config.combat.voipdrop = ReadCombatSwitch(cfg, kCombatVoipdropKey, r);/  r.config.combat.voipdrop = ReadCombatSwitch(cfg, kCombatVoipdropKey, r, false);/" quest-host-shared quest_config_test
runm vd_qc_key_typo        $QC "s/kCombatVoipdropKey = \"combat.voipdrop\";/kCombatVoipdropKey = \"combat.voipdrops\";/" quest-host-shared quest_config_test
runm vd_qc_not_assigned    $QC "s/  r.config.combat.voipdrop = ReadCombatSwitch(cfg, kCombatVoipdropKey, r);/  (void)ReadCombatSwitch(cfg, kCombatVoipdropKey, r);/" quest-host-shared quest_config_test
runm vd_nc_known_key       src/core/nevr_config.cpp "s/\"fxtrail\", \"voipdrop\",/\"fxtrail\",/" quest-host-shared quest_config_test
# the free head is a plain load; a null receiver goes to the game's function untouched
runm vd_plain_null_unchecked $F "s/  const std::int32_t head = self != nullptr ? \*reinterpret_cast<const volatile std::int32_t\*>(cs + kFreeHead) : 0;/  const std::int32_t head = *reinterpret_cast<const volatile std::int32_t*>(cs + kFreeHead);/"
runm vd_plain_load_wrong_off $F "s/\*reinterpret_cast<const volatile std::int32_t\*>(cs + kFreeHead)/*reinterpret_cast<const volatile std::int32_t*>(cs + kFreeHead + 4)/"

runm vd_row_member         $CI "s/\"combat.voipdrop\", \&nevr_quest::CombatConfig::voipdrop,/\"combat.voipdrop\", \&nevr_quest::CombatConfig::custctx,/" quest-host-combat combat_install_test
runm vd_row_key            $CI "s/\"combat.voipdrop\", \&nevr_quest/\"combat.voipdrops\", \&nevr_quest/" quest-host-combat combat_install_test
# voipdrop asks nothing of the scope: it is not a reader of the scope row
runm vd_reader_listed      $CI "s/ || g_config.custctx.on/ || g_config.custctx.on || g_config.voipdrop.on/" quest-host-combat combat_install_test
# (kTable, kFreeHead, kPageBytes, kPageBlock and kVtAlloc are tied to the pinned words by static_assert in voipdrop_targets.h: a changed one fails the build)
runm vd_vtfree_off        $T "s/kVtFree = 0x38;/kVtFree = 0x30;/"
# every cap says when it stops (line_budget.h is judged here and by the other lists through the notice each test asserts)
LB=src/quest/combat/line_budget.h
runm vd_drop_cap_ignored    $F "s/  if (LineBudget(\&g_dropLines, kDropLines,/  if (true || LineBudget(\&g_dropLines, kDropLines,/"
runm lb_notice_off_by_one   $LB "s/  if (n == cap + 1) {/  if (n == cap) {/"
runm lb_notice_every_time   $LB "s/  if (n == cap + 1) {/  if (n > cap) {/"
runm lb_budget_off_by_one   $LB "s/  return n <= cap;/  return n < cap;/"
runm lb_budget_always       $LB "s/  return n <= cap;/  return true;/"
runm lb_notice_quiet        $LB "s/sentinel::LogLevel::kWarn, event,/sentinel::LogLevel::kInfo, event,/"
runm lb_cap_field           $LB "s/{\"cap\", cap}/{\"cap\", cap + 1}/"

runm vd_total_cap_ignored   $F "s/             LineBudget(\&g_totals, kTotalLines,/             LineBudget(\&g_totals, 1000000,/"