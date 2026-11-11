# bullet_velocity_guard: each rule of src/quest/combat/bullet_velocity_guard.cpp and src/runtime/combat/bullet_velocity_guard_rules.h removed or bent
# once; src/quest/tests/bullet_velocity_guard_test.cpp must fail on every one. Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
F=src/quest/combat/bullet_velocity_guard.cpp
H=src/quest/combat/bullet_velocity_guard.h
R=src/runtime/combat/bullet_velocity_guard_rules.h
# the rule
runm bvg_gt_ge           $R "s/  return !(v2 > kMinSpeedSquared) || !(v2 <= kMaxFinite);/  return !(v2 >= kMinSpeedSquared) || !(v2 <= kMaxFinite);/"
runm bvg_threshold       $R "s/kMinSpeedSquared = 1e-6f;/kMinSpeedSquared = 1e-7f;/"
runm bvg_no_finite       $R "s/  return !(v2 > kMinSpeedSquared) || !(v2 <= kMaxFinite);/  return !(v2 > kMinSpeedSquared);/"
runm bvg_no_null         $R "s/  if (v == nullptr) return true;/  if (v == nullptr) return false;/"
runm bvg_no_z            $R "s/v\[0\] \* v\[0\] + v\[1\] \* v\[1\] + v\[2\] \* v\[2\]/v[0] * v[0] + v[1] * v[1]/"
# the handler: a skip never calls FireBullet; a pass gives it the same eight arguments and its result
runm bvg_skip_calls      $F "s/    return 0;\$/    return original(bcs, handle, gun, damage, quat, vel, remote, flag);/"
runm bvg_arg_swap        $F "s/  return original(bcs, handle, gun, damage, quat, vel, remote, flag);/  return original(bcs, handle, gun, damage, vel, quat, remote, flag);/"
runm bvg_arg_flag        $F "s/  return original(bcs, handle, gun, damage, quat, vel, remote, flag);/  return original(bcs, handle, gun, damage, quat, vel, remote, flag + 1);/"
runm bvg_result_dropped  $F "s/  return original(bcs, handle, gun, damage, quat, vel, remote, flag);/  original(bcs, handle, gun, damage, quat, vel, remote, flag);\n  return 0;/"
runm bvg_never_skips     $F "s/  if (rules::Skips(v)) {/  if (false \&\& rules::Skips(v)) {/"
# the install: the slot must hold FireBullet, the words gate it, the build ID gates it
runm bvg_no_expected     $F "s/dronemuzzle::Target(targets::kFireSymbol, pins.slot, image.base, pins.fn)/sentinel::GotTarget(sentinel::pinned::kLibR15, targets::kFireSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), pins.slot)/"
runm bvg_words_skipped   $F "s/  if (pinned::FirstFailure(image.base, \&targets::kWordPins, 1, \&word))/  if (false \&\& pinned::FirstFailure(image.base, \&targets::kWordPins, 1, \&word))/"
runm bvg_build_id        $F "s/  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\", pins.slot);//"
runm bvg_refusal_info    $F "s/LogLevel::kError, \"combat_bullet_velocity_guard_install\"/LogLevel::kInfo, \"combat_bullet_velocity_guard_install\"/g"
runm bvg_off_ignored     $F "s/  if (!on) {/  if (!on \&\& false) {/"
# the counts and the lines
runm bvg_skip_uncounted  $F "s/g_skipped.fetch_add(1, std::memory_order_relaxed) + 1/g_skipped.fetch_add(0, std::memory_order_relaxed) + 1/"
runm bvg_calls_uncounted $F "s/  g_calls.fetch_add(1, std::memory_order_relaxed);/  (void)g_calls;/"
runm bvg_skip_debug      $F "s/LogLevel::kInfo, \"combat_bullet_velocity_guard_skip\",/LogLevel::kDebug, \"combat_bullet_velocity_guard_skip\",/g"
runm bvg_skip_lines      $H "s/kSkipLines = 10;/kSkipLines = 11;/"
runm bvg_skip_every      $H "s/kSkipEveryMs = 10000;/kSkipEveryMs = 9000;/"
runm bvg_no_count_line   $F "s/  } else if (l.verdict == nevr::BurstThenCount::Verdict::kCount) {/  } else if (false) {/"
runm bvg_index_full      $F "s/static_cast<long long>(handle \& 0xffff)/static_cast<long long>(handle)/"
runm bvg_total_always    $F "s/  if (!g_hook.installed()) return;/  (void)g_hook;/"
runm bvg_total_never     $F "s/  if (!g_hook.installed()) return;/  return;/"
# dronemuzzle's drone-shot test is the same rule (one definition)
DM=src/quest/combat/dronemuzzle.cpp
runm bvg_dm_rule_ignored $DM "s/  if (::nevr_combat::bullet_velocity_guard::Skips(v)) return r;/  if (v == nullptr) return r;/" quest-host-combat dronemuzzle_test
# The table row (combat_install.cpp) and the key (quest_config.cpp, nevr_config.cpp) are shared files, run against combat_install_test and quest_config_test.
CI=src/quest/combat/combat_install.cpp
runm bvg_row_ignores_switch $CI "s/bullet_velocity_guard::Install(g_config.bulletVelocityGuard.on)/bullet_velocity_guard::Install(true)/" quest-host-combat combat_install_test
runm bvg_row_end_null       $CI "s/bulletVelocityGuard, \&bullet_velocity_guard::NoteEnd}/bulletVelocityGuard, nullptr}/" quest-host-combat combat_install_test
runm bvg_row_below_dm       $CI "/^    Module{\"bullet_velocity_guard\", /{h;d};/^    Module{\"dronemuzzle\", /{p;x}" quest-host-combat combat_install_test
QC=src/quest/sentinel/quest_config.cpp
runm bvg_qc_default_off     $QC "s/  r.config.combat.bulletVelocityGuard = ReadCombatSwitch(cfg, kCombatBulletVelocityGuardKey, r);/  r.config.combat.bulletVelocityGuard = ReadCombatSwitch(cfg, kCombatBulletVelocityGuardKey, r, false);/" quest-host-shared quest_config_test
runm bvg_qc_not_assigned    $QC "s/  r.config.combat.bulletVelocityGuard = ReadCombatSwitch(cfg, kCombatBulletVelocityGuardKey, r);/  (void)ReadCombatSwitch(cfg, kCombatBulletVelocityGuardKey, r);/" quest-host-shared quest_config_test
runm bvg_nc_known_key       src/core/nevr_config.cpp "s/\"dronemuzzle\", \"bullet_velocity_guard\",/\"dronemuzzle\",/" quest-host-shared quest_config_test
