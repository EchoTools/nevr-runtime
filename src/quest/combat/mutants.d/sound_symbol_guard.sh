# sound_symbol_guard: each rule of src/quest/combat/sound_symbol_guard.cpp and src/runtime/combat/sound_symbol_guard_rules.h removed or bent once;
# src/quest/tests/sound_symbol_guard_test.cpp must fail on every one. Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
F=src/quest/combat/sound_symbol_guard.cpp
H=src/quest/combat/sound_symbol_guard.h
R=src/runtime/combat/sound_symbol_guard_rules.h
# the rule
runm ssg_no_zero          $R "s/  if (symbol == kInvalidSymbol) return Drop::kZero;//"
runm ssg_default_kept     $R "s/^    0x1406b30aU,\$/    0x1406b30bU,/"
runm ssg_classes_swapped  $R "s/  if (symbol == kInvalidSymbol) return Drop::kZero;/  if (symbol == kInvalidSymbol) return Drop::kUnregistered;/"
runm ssg_unreg_ignored    $R "s/    if (kUnregistered\[i\] == symbol) return Drop::kUnregistered;/    if (kUnregistered[i] == symbol) return Drop::kNone;/"
# the handler: a drop never calls Play3DSound and answers 0; a pass gives it the same four arguments and its answer
runm ssg_drop_minus_one   $F "s/  return 0;  \/\/ Play3DSound's own answer/  return -1;  \/\/ Play3DSound's own answer/"
runm ssg_drop_calls       $F "s/  return 0;  \/\/ Play3DSound's own answer/  return original(player, symbol, emitter, volume);  \/\/ Play3DSound's own answer/"
runm ssg_never_drops      $F "s/  if (d == rules::Drop::kNone) return original(player, symbol, emitter, volume);/  if (true) return original(player, symbol, emitter, volume);/"
runm ssg_full_symbol      $F "s/  const std::uint32_t sym = static_cast<std::uint32_t>(symbol);/  const std::uint32_t sym = symbol == static_cast<std::uint32_t>(symbol) ? static_cast<std::uint32_t>(symbol) : 1U;/"
runm ssg_arg_symbol       $F "s/  if (d == rules::Drop::kNone) return original(player, symbol, emitter, volume);/  if (d == rules::Drop::kNone) return original(player, sym, emitter, volume);/"
runm ssg_arg_emitter      $F "s/  if (d == rules::Drop::kNone) return original(player, symbol, emitter, volume);/  if (d == rules::Drop::kNone) return original(player, symbol, emitter + 1, volume);/"
runm ssg_result_dropped   $F "s/  if (d == rules::Drop::kNone) return original(player, symbol, emitter, volume);/  if (d == rules::Drop::kNone) { original(player, symbol, emitter, volume); return 0x78; }/"
# the caller gate: only PlaySound's and the bullet site's posts are dropped
runm ssg_other_dropped    $F "s/  if (site == Caller::kOther) {/  if (false \&\& site == Caller::kOther) {/"
runm ssg_other_uncounted  $F "s/    g_keptOther.fetch_add(1, std::memory_order_relaxed);/    (void)g_keptOther;/"
runm ssg_caller_swapped   $H "s/? Caller::kPlaySound : ret == base + pins.bulletSiteReturn ? Caller::kBulletSite/? Caller::kBulletSite : ret == base + pins.bulletSiteReturn ? Caller::kPlaySound/"
runm ssg_caller_no_base   $H "s/  return ret == base + pins.playSoundReturn/  return ret == pins.playSoundReturn/"
runm ssg_play_return      $H "s/0x170698cULL, 0x21230d4ULL}/0x1706988ULL, 0x21230d4ULL}/"
runm ssg_bullet_return    $H "s/0x170698cULL, 0x21230d4ULL}/0x170698cULL, 0x21230d0ULL}/"
# the scope (README rule 9): outside a combat level, and on a stale answer, the game's function and nothing else
runm ssg_no_scope         $F "s/  if (!scope::SeenCombatOrLobby()) return original(player, symbol, emitter, volume);/  (void)0;/"
runm ssg_no_fresh         $F "s/  if (!scope::LookIsFresh()) return original(player, symbol, emitter, volume);/  (void)0;/"
runm ssg_calls_before_scope $F "s/  if (!scope::SeenCombatOrLobby()) return original(player, symbol, emitter, volume);/  g_calls.fetch_add(1, std::memory_order_relaxed);\n  if (!scope::SeenCombatOrLobby()) return original(player, symbol, emitter, volume);/"
runm ssg_scope_live       $F "s/  if (!scope::Live()) return Refused(\"scope_not_live\", pins.slot);//"
# the install: the slot must hold Play3DSound, the words and records gate it, the build ID gates it
runm ssg_no_expected      $F "s/dronemuzzle::Target(targets::kPlay3DSymbol, pins.slot, image.base, pins.fn)/sentinel::GotTarget(sentinel::pinned::kLibR15, targets::kPlay3DSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), pins.slot)/"
runm ssg_words_skipped    $F "s/  if (pinned::FirstFailure(image.base, kInstallPins,/  if (false \&\& pinned::FirstFailure(image.base, kInstallPins,/"
runm ssg_drop_pins_skipped $F "s/constexpr pinned::Pins kInstallPins\[\] = {targets::kWordPins, targets::kDropPins};/constexpr pinned::Pins kInstallPins[] = {targets::kWordPins};/"
runm ssg_build_id         $F "s/  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\", pins.slot);//"
runm ssg_refusal_info     $F "s/LogLevel::kError, \"combat_sound_symbol_guard_install\"/LogLevel::kInfo, \"combat_sound_symbol_guard_install\"/g"
runm ssg_off_ignored      $F "s/  if (!on) {/  if (!on \&\& false) {/"
# the counts and the lines
runm ssg_class_counter_swapped $F "s/d == rules::Drop::kZero ? g_droppedZero : g_droppedUnregistered/d == rules::Drop::kZero ? g_droppedUnregistered : g_droppedZero/"
runm ssg_calls_uncounted  $F "s/  g_calls.fetch_add(1, std::memory_order_relaxed);/  (void)g_calls;/"
runm ssg_drop_info        $F "s/LogLevel::kDebug, \"combat_sound_symbol_guard_drop\",/LogLevel::kInfo, \"combat_sound_symbol_guard_drop\",/"
runm ssg_drops_info       $F "s/LogLevel::kDebug, \"combat_sound_symbol_guard_drops\",/LogLevel::kInfo, \"combat_sound_symbol_guard_drops\",/g"
runm ssg_cap_17           $H "s/kDropLines = 16;/kDropLines = 17;/"
runm ssg_every            $H "s/kDropEveryMs = 10000;/kDropEveryMs = 9000;/"
runm ssg_no_count_line    $F "s/  } else if (l.verdict == nevr::BurstThenCount::Verdict::kCount) {/  } else if (false) {/"
runm ssg_total_always     $F "s/  if (!g_hook.installed()) return;/  (void)g_hook;/"
runm ssg_total_never      $F "s/  if (!g_hook.installed()) return;/  return;/"
# The table row and ScopeNormal (combat_install.cpp) and the key (quest_config.cpp, nevr_config.cpp) are shared files, run against combat_install_test
# and quest_config_test.
CI=src/quest/combat/combat_install.cpp
runm ssg_row_ignores_switch $CI "s/sound_symbol_guard::Install(g_config.soundSymbolGuard.on)/sound_symbol_guard::Install(true)/" quest-host-combat combat_install_test
runm ssg_row_end_null       $CI "s/soundSymbolGuard, \&sound_symbol_guard::NoteEnd}/soundSymbolGuard, nullptr}/" quest-host-combat combat_install_test
runm ssg_scope_reader_gone  $CI "s/ || g_config.soundSymbolGuard.on;/;/" quest-host-combat combat_install_test
QC=src/quest/sentinel/quest_config.cpp
runm ssg_qc_default_on      $QC "s/  r.config.combat.soundSymbolGuard = ReadCombatSwitch(cfg, kCombatSoundSymbolGuardKey, r, false);/  r.config.combat.soundSymbolGuard = ReadCombatSwitch(cfg, kCombatSoundSymbolGuardKey, r);/" quest-host-shared quest_config_test
runm ssg_qc_not_assigned    $QC "s/  r.config.combat.soundSymbolGuard = ReadCombatSwitch(cfg, kCombatSoundSymbolGuardKey, r, false);/  (void)ReadCombatSwitch(cfg, kCombatSoundSymbolGuardKey, r, false);/" quest-host-shared quest_config_test
runm ssg_nc_known_key       src/core/nevr_config.cpp "s/\"bullet_velocity_guard\", \"sound_symbol_guard\",/\"bullet_velocity_guard\",/" quest-host-shared quest_config_test
