# frozen_frame_exit: each rule of src/quest/combat/frozen_frame_exit.cpp, frozen_frame_exit.h and frozen_frame_exit_rules.h removed or bent once;
# src/quest/tests/frozen_frame_exit_test.cpp must fail on every one. Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
F=src/quest/combat/frozen_frame_exit.cpp
H=src/quest/combat/frozen_frame_exit.h
R=src/quest/combat/frozen_frame_exit_rules.h
# the rule
runm ffe_no_invr          $R "s/  if (lastSubmitMs == 0 || !inVr) return false;/  if (lastSubmitMs == 0 \&\& inVr) return false;/"
runm ffe_no_unarmed       $R "s/  if (lastSubmitMs == 0 || !inVr) return false;/  if (!inVr) return false;/"
runm ffe_gt_ge            $R "s/  return nowMs - lastSubmitMs > static_cast<std::int64_t>(limitS) \* 1000;/  return nowMs - lastSubmitMs >= static_cast<std::int64_t>(limitS) * 1000;/"
runm ffe_default_limit    $R "s/kDefaultLimitS = 30;/kDefaultLimitS = 31;/"
# the observer: the scope gate first, the stamp, the arm count
runm ffe_no_gate          $H "s/  if (!Watching()) return;/  if (false) return;/"
runm ffe_no_scope         $H "s/{ return scope::SeenCombatOrLobby() \&\& detail::g_armed.load(std::memory_order_relaxed); }/{ return detail::g_armed.load(std::memory_order_relaxed); }/"
runm ffe_no_armed         $H "s/{ return scope::SeenCombatOrLobby() \&\& detail::g_armed.load(std::memory_order_relaxed); }/{ return scope::SeenCombatOrLobby(); }/"
runm ffe_arms_every       $F "s/  if (g_lastSubmitMs.exchange(now, std::memory_order_acq_rel) == 0) {/  if (g_lastSubmitMs.exchange(now, std::memory_order_acq_rel) == 0 || true) {/"
runm ffe_submits_uncounted $F "s/  g_submits.fetch_add(1, std::memory_order_relaxed);/  (void)g_submits;/"
# the watchdog: disarm, the second read, fire once, the order of the steps
runm ffe_no_disarm        $F "s/    if (last != 0) g_lastSubmitMs.compare_exchange_strong(last, 0, std::memory_order_acq_rel);/    (void)last;/"
runm ffe_no_word_test     $F "s/  if (ReadInVrWord() == 0 || !scope::SeenCombatOrLobby()) {/  if (!scope::SeenCombatOrLobby()) {/"
runm ffe_no_scope_tick    $F "s/  if (ReadInVrWord() == 0 || !scope::SeenCombatOrLobby()) {/  if (ReadInVrWord() == 0) {/"
runm ffe_no_double_check  $F "s/  if (!rules::Expired(now, last, inVr, limitS)) return;/  (void)inVr;/"
runm ffe_fires_again      $F "s/  if (g_fired.load(std::memory_order_acquire)) return;/  (void)g_fired;/"
runm ffe_finish_first     $F "s/  const bool canFinish = g_canFinish.load(std::memory_order_acquire)();/  const bool canFinish = g_canFinish.load(std::memory_order_acquire)();\n  if (canFinish) g_finish.load(std::memory_order_acquire)();/"
runm ffe_no_finish        $F "s/  if (canFinish) g_finish.load(std::memory_order_acquire)();/  (void)canFinish;/"
runm ffe_no_grace         $F "s/  g_grace.load(std::memory_order_acquire)(kGraceMs);/  (void)kGraceMs;/"
runm ffe_no_end           $F "s/  g_end.load(std::memory_order_acquire)(process_end::kFailureExitCode, \"frozen_frame\");/  (void)0;/"
runm ffe_end_code_0       $F "s/(process_end::kFailureExitCode, \"frozen_frame\")/(0, \"frozen_frame\")/"
runm ffe_grace_ms         $H "s/kGraceMs = 5000;/kGraceMs = 4000;/"
runm ffe_fired_info       $F "s/LogLevel::kWarn, \"combat_frozen_frame_exit\",/LogLevel::kInfo, \"combat_frozen_frame_exit\",/"
runm ffe_no_action        $F "s/{\"action\", canFinish ? \"finish_activity\" : \"no_activity\"}/{\"action\", \"finish_activity\"}/"
# the end of the process: join unless ending
runm ffe_no_stop          $F "s/  const bool joined = StopThread();/  const bool joined = true;/"
runm ffe_join_ending      $F "s/    const bool ending = g_ending.load(std::memory_order_acquire);/    const bool ending = false;/"
runm ffe_counts_always    $F "s/  if (!detail::g_armed.load(std::memory_order_acquire)) return;/  (void)0;/"
# the install: off, the scope, the submit slot, the build ID, the words, the limit's range, the refusal level
runm ffe_off_ignored      $F "s/  if (!on) {/  if (!on \&\& false) {/"
runm ffe_scope_ignored    $F "s/  if (!scope::Live()) return Refused(\"scope_not_live\", limitS, limitFromFile);//"
runm ffe_slot_ignored     $F "s/  if (!render_config::SubmitSlotHeld()) return Refused(\"submit_slot_not_held\", limitS, limitFromFile);//"
runm ffe_build_id         $F "s/  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused(\"word_unreadable\", limitS, limitFromFile, \"build_id_mismatch\");//"
runm ffe_words_skipped    $F "s/  if (pinned::FirstFailure(image.base, \&targets::kWordPins, 1, \&word)) {/  if (false \&\& pinned::FirstFailure(image.base, \&targets::kWordPins, 1, \&word)) {/"
runm ffe_no_range         $F "s/  if (limitS < rules::kMinLimitS || limitS > rules::kMaxLimitS) {/  if (false) {/"
runm ffe_refusal_info     $F "s/LogLevel::kError, \"combat_frozen_frame_exit_install\"/LogLevel::kInfo, \"combat_frozen_frame_exit_install\"/g"
runm ffe_never_armed      $F "s/  detail::g_armed.store(true, std::memory_order_release);\$/  (void)0;/"
# The table row and the scope reader (combat_install.cpp, a shared file) and the limit's key (quest_config.cpp), run against their own programs.
CI=src/quest/combat/combat_install.cpp
runm ffe_row_ignores_switch $CI "s/return frozen_frame_exit::Install(g_config.frozenFrameExit.on, /return frozen_frame_exit::Install(true, /" quest-host-combat combat_install_test
runm ffe_row_ignores_limit  $CI "s/return frozen_frame_exit::Install(g_config.frozenFrameExit.on, g_config.frozenFrameExitS.value,/return frozen_frame_exit::Install(g_config.frozenFrameExit.on, 30,/" quest-host-combat combat_install_test
runm ffe_not_a_scope_reader $CI "s/ || g_config.frozenFrameExit.on;/;/" quest-host-combat combat_install_test
QC=src/quest/sentinel/quest_config.cpp
runm ffe_limit_range        $QC "s/kCombatFrozenFrameExitSKey, 10, 900, 30, r)/kCombatFrozenFrameExitSKey, 10, 901, 30, r)/" quest-host-shared quest_config_test
runm ffe_key_unread         $QC "s/  r.config.combat.frozenFrameExit = ReadCombatSwitch(cfg, kCombatFrozenFrameExitKey, r);//" quest-host-shared quest_config_test
