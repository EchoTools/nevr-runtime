# shieldgamma: each rule of src/quest/combat/shieldgamma.cpp and shieldgamma_targets.h removed or bent once; src/quest/tests/shieldgamma_test.cpp must fail
# on every one, and so must the test of each line outside the module that carries its switches, its record or its pins. Sourced by ../mutants.sh, which
# defines `runm <name> <file> <sed> [<build dir> <test target>]`.
C=src/quest/combat/shieldgamma.cpp
T=src/quest/combat/shieldgamma_targets.h
I=src/quest/combat/combat_install.cpp
# the handler is the arena path: the scope test, then the game's function once with the same arguments
runm sg_scope_ignored       $C "s/^  if (!scope::SeenCombatOrLobby()) return original(node, data);\$//"
runm sg_scope_inverted      $C "s/if (!scope::SeenCombatOrLobby()) return original(node, data);/if (scope::SeenCombatOrLobby()) return original(node, data);/"
runm sg_arena_result        $C "s/if (!scope::SeenCombatOrLobby()) return original(node, data);/if (!scope::SeenCombatOrLobby()) return original(node, data) + 1;/"
runm sg_stale_believed      $C "s/^  if (!scope::LookIsFresh()) return original(node, data);.*\$//"
runm sg_stale_inverted      $C "s/if (!scope::LookIsFresh()) return original(node, data);/if (scope::LookIsFresh()) return original(node, data);/"
# the combat call
runm sg_mat_ignored         $C "s/At<std::uint64_t>(d, kDataMaterial) != kMaterial || //"
runm sg_type_ignored        $C "s/ || At<std::int32_t>(d, kDataType) != kVec4//"
runm sg_space_ignored       $C "s/ || At<std::int32_t>(d, kDataSpace) != kLinear//"
runm sg_gamma_exponent      $T "s/kGamma = 2.2f/kGamma = 2.0f/"
runm sg_no_clamp_hi         $C "s/(x > 1.0f ? 1.0f : x)/x/"
runm sg_no_clamp_lo         $C "s/x < 0.0f ? 0.0f : //"
runm sg_no_restore          $C "s/^  std::memcpy(value, was, sizeof was);\$/  (void)was;/"
runm sg_alpha_written       $C "s/std::memcpy(value, now, sizeof now);/std::memcpy(value, now, sizeof now); std::memset(value + 12, 0, 4);/"
runm sg_gamma_flag_ignored  $C "s/^  if (!g_gamma.load(std::memory_order_relaxed)) return original(node, data);\$//"
runm sg_mirror_flag_ignored $C "s/g_mirror.load(std::memory_order_relaxed) \\&\\& prop == kRingProperty/prop == kRingProperty/"
runm sg_mirror_any_prop     $C "s/ \\&\\& prop == kRingProperty//"
runm sg_mirror_same_mat     $C "s/^    std::memcpy(copy + kDataMaterial, \\&kTubeMaterial, sizeof kTubeMaterial);\$//"
runm sg_mirror_undecoded    $C "s/^    std::memcpy(copy + kDataValue, now, sizeof now);\$//"
runm sg_mirror_on_orig      $C "s/^    original(node, copy);\$/    original(node, data);/"
runm sg_mirror_result       $C "s/^    original(node, copy);\$/    return original(node, copy);/"
runm sg_mirror_skipped      $C "s/^    original(node, copy);\$/    (void)copy;/"
runm sg_unreadable_drops    $C "s/g_unreadable.fetch_add(1, std::memory_order_relaxed);/g_unreadable.fetch_add(1, std::memory_order_relaxed); return 0;/"
runm sg_decode_unthinned    $C "s/if (n \\& (n - 1)) return;.*\$//"
# the install: both switches off installs nothing, refused before the hook when the scope cannot change, the refusal an Error with its switches
runm sg_off_installs        $C "s/if (!gamma \\&\\& !tubeMirror) {/if (false) {/"
runm sg_scope_not_asked     $C "s/if (!scope::Live()) return Refused/if (false) return Refused/"
runm sg_scope_asked_after   $C "s/^  if (!scope::Live()) return Refused.*\$//; s/^\\(  if (status == sentinel::GotStatus::kOk) status = sentinel::InstallThunk<Thunk>.*\\)\$/\\1\n  if (!scope::Live()) return Refused(\"scope_not_live\", \"x\", gamma, tubeMirror);/"
runm sg_refusal_quiet       $C "s/sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_shieldgamma_install\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_shieldgamma_install\"/"
# the pins
runm sg_wrong_slot          $T "s/kSlotVaddr = 0x36d2020ULL/kSlotVaddr = 0x36d2028ULL/"
runm sg_wrong_fn            $T "s/kFnVaddr = 0x272d55cULL/kFnVaddr = 0x272d560ULL/"
runm sg_wrong_mat           $T "s/kMaterial = 0xdbde1b05fa8f6200ULL/kMaterial = 0xdbde1b05fa8f6201ULL/"
runm sg_wrong_tube          $T "s/kTubeMaterial = 0xb28e940eb21a5798ULL/kTubeMaterial = 0xb28e940eb21a5799ULL/"
runm sg_wrong_ring          $T "s/kRingProperty = 0xdd35e419672ee0baULL/kRingProperty = 0xdd35e419672ee0bbULL/"
runm sg_original_unchecked  $T "s/^  target.expectedOriginal = .*\$/  (void)base;/"
runm sg_target_unowned      $T "s/return target.Owned(\"combat_shieldgamma\");/return target;/"
# the table row and the record that carry the switches
runm sg_row_args_swapped    $I "s/shieldgamma::Install(g_config.shieldgammaGamma.on, g_config.shieldgammaTubeMirror.on)/shieldgamma::Install(g_config.shieldgammaTubeMirror.on, g_config.shieldgammaGamma.on)/" quest-host-combat combat_install_test
runm sg_gamma_row_key      $I "s/\"combat.shieldgamma_gamma\", \&nevr_quest::CombatConfig::shieldgammaGamma}/\"combat.shieldgamma_gama\", \&nevr_quest::CombatConfig::shieldgammaGamma}/" quest-host-combat combat_install_test
runm sg_gamma_row_member   $I "s/\"combat.shieldgamma_gamma\", \&nevr_quest::CombatConfig::shieldgammaGamma}/\"combat.shieldgamma_gamma\", \&nevr_quest::CombatConfig::shieldgammaTubeMirror}/" quest-host-combat combat_install_test
runm sg_mirror_row_key     $I "s/\"combat.shieldgamma_tube_mirror\", \&nevr_quest::CombatConfig::shieldgammaTubeMirror}/\"combat.shieldgamma_tube_mirro\", \&nevr_quest::CombatConfig::shieldgammaTubeMirror}/" quest-host-combat combat_install_test
runm sg_mirror_row_member  $I "s/\"combat.shieldgamma_tube_mirror\", \&nevr_quest::CombatConfig::shieldgammaTubeMirror}/\"combat.shieldgamma_tube_mirror\", \&nevr_quest::CombatConfig::shieldgammaGamma}/" quest-host-combat combat_install_test
runm sg_gamma_row_missing  $I "s/^    Module{\"shieldgamma_gamma\", nullptr, nullptr, \"combat.shieldgamma_gamma\", \&nevr_quest::CombatConfig::shieldgammaGamma},\$/    Module{\"shieldgamma_gamma\", nullptr, nullptr},/" quest-host-combat combat_install_test
# the keys: read, defaulted as the module needs, known to the schema
runm sg_gamma_default_on    src/quest/sentinel/quest_config.h "s/CombatSwitch shieldgammaGamma{false, Source::kAbsent}/CombatSwitch shieldgammaGamma{true, Source::kAbsent}/" quest-host-shared quest_config_test
runm sg_mirror_default_off  src/quest/sentinel/quest_config.h "s/CombatSwitch shieldgammaTubeMirror{true, Source::kAbsent}/CombatSwitch shieldgammaTubeMirror{false, Source::kAbsent}/" quest-host-shared quest_config_test
runm sg_gamma_fallback      src/quest/sentinel/quest_config.cpp "s/ReadCombatSwitch(cfg, kCombatShieldGammaKey, r, \\/\\*fallback=\\*\\/false)/ReadCombatSwitch(cfg, kCombatShieldGammaKey, r)/" quest-host-shared quest_config_test
runm sg_mirror_reads_gamma  src/quest/sentinel/quest_config.cpp "s/ReadCombatSwitch(cfg, kCombatShieldTubeMirrorKey, r)/ReadCombatSwitch(cfg, kCombatShieldGammaKey, r)/" quest-host-shared quest_config_test
