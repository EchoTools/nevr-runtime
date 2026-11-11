# hudmeter: each rule of src/quest/combat/hudmeter.cpp and hudmeter_targets.h removed or bent once; src/quest/tests/hudmeter_test.cpp must fail on every
# one, and so must the test of each line outside the module that carries its switch, its record or its pins. Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`.
C=src/quest/combat/hudmeter.cpp
T=src/quest/combat/hudmeter_targets.h
I=src/quest/combat/combat_install.cpp
# the handler is the arena path: the scope test, then the game's function with the same arguments
runm hm_scope_ignored       $C "s/^  if (!scope::SeenCombatOrLobby()) return original(node, data);\$//"
runm hm_scope_inverted      $C "s/if (!scope::SeenCombatOrLobby()) return original(node, data);/if (scope::SeenCombatOrLobby()) return original(node, data);/"
runm hm_arena_null_data     $C "s/if (!scope::SeenCombatOrLobby()) return original(node, data);/if (!scope::SeenCombatOrLobby()) return original(node, nullptr);/"
runm hm_stale_believed      $C "s/^  if (!scope::LookIsFresh()) return original(node, data);.*\$//"
runm hm_stale_inverted      $C "s/if (!scope::LookIsFresh()) return original(node, data);/if (scope::LookIsFresh()) return original(node, data);/"
# the combat call
runm hm_item_ignored        $C "s/At<std::uint64_t>(d, kDataItem) != kItem || //"
runm hm_item_off            $C "s/At<std::uint64_t>(d, kDataItem) != kItem/At<std::uint64_t>(d, kDataItem + 8) != kItem/"
runm hm_sizeflag_ignored    $C "s/ || At<std::int32_t>(d, kDataSizeOn) == 0//"
runm hm_only_lobby_comp     $C "s/if (At<std::uint64_t>(d, kDataComponent) != kLobbyComponent) LogOtherComp(At<std::uint64_t>(d, kDataComponent));/if (At<std::uint64_t>(d, kDataComponent) != kLobbyComponent) { (void)\\&LogOtherComp; return original(node, data); }/"
runm hm_no_size_restore     $C "s/std::memcpy(size, d + kDataSize, sizeof kFull);.*\$/(void)size;/"
runm hm_no_full_write       $C "s/^  std::memcpy(size, kFull, sizeof kFull);\$/  (void)kFull;/"
runm hm_full_swapped        $C "s/kFull\\[2\\] = {kFullX, kFullY}/kFull[2] = {kFullY, kFullX}/"
runm hm_half_zero           $C "s/if (x != 0.0f || y != 0.0f) {/if (x != 0.0f \\&\\& y != 0.0f) {/"
runm hm_unreadable_drops    $C "s/g_unreadable.fetch_add(1, std::memory_order_relaxed);/g_unreadable.fetch_add(1, std::memory_order_relaxed); return 0;/"
runm hm_result_dropped      $C "s/^  return result;\$/  return result + 1;/"
runm hm_pass_repeats        $C "s/^  g_passed.store(false, std::memory_order_relaxed);.*\$//"
runm hm_full_unthinned      $C "s/if (n \\& (n - 1)) return;.*\$//"
runm hm_fill_uncapped       $C "s/>= kFillLines) return;/>= (kFillLines + 100000u)) return;/"
runm hm_comp_named_often    $C "s/if (!g_otherComp.compare_exchange_strong(none, comp, std::memory_order_relaxed)) return;/(void)none;/"
# the install: off, refused before the hook when the scope cannot change, the refusal an Error
runm hm_off_installs        $C "s/^  if (!on) {\$/  if (!on \\&\\& lookup == nullptr) {/"
runm hm_off_not_counted     $C "s/    return {false, \"off\", true};/    return {false, \"off\"};/"
runm hm_scope_not_asked     $C "s/if (!scope::Live()) return Refused/if (false) return Refused/"
runm hm_scope_asked_after   $C "s/^  if (!scope::Live()) return Refused.*\$//; s/^\\(  if (status == sentinel::GotStatus::kOk) status = sentinel::InstallThunk<Thunk>.*\\)\$/\\1\n  if (!scope::Live()) return Refused(\"scope_not_live\", \"x\");/"
runm hm_refusal_quiet       $C "s/sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, \"combat_hudmeter_install\"/sentinel::LogFields(sentinel::LogLevel::kInfo, \"combat_hudmeter_install\"/"
# the pins
runm hm_wrong_slot          $T "s/kSlotVaddr = 0x36c6ce8ULL/kSlotVaddr = 0x36c6cf0ULL/"
runm hm_wrong_fn            $T "s/kFnVaddr = 0x2726eacULL/kFnVaddr = 0x2726eb0ULL/"
runm hm_wrong_item          $T "s/kItem = 0x9fd2e8cc0e46df3cULL/kItem = 0x9fd2e8cc0e46df3dULL/"
runm hm_wrong_full          $T "s/kFullX = 86.0f, kFullY = 88.0f/kFullX = 86.0f, kFullY = 80.0f/"
runm hm_original_unchecked  $T "s/^  target.expectedOriginal = .*\$/  (void)base;/"
runm hm_target_unowned      $T "s/return target.Owned(\"combat_hudmeter\");/return target;/"
# the table row and the record that carry the switch
runm hm_row_switch_true     $I "s/hudmeter::Install(g_config.hudmeter.on)/hudmeter::Install(true)/" quest-host-combat combat_install_test
runm hm_row_key            $I "s/\"combat.hudmeter\", \&nevr_quest::CombatConfig::hudmeter}/\"combat.hudmete\", \&nevr_quest::CombatConfig::hudmeter}/" quest-host-combat combat_install_test
runm hm_row_member         $I "s/\"combat.hudmeter\", \&nevr_quest::CombatConfig::hudmeter}/\"combat.hudmeter\", \&nevr_quest::CombatConfig::shieldgammaTubeMirror}/" quest-host-combat combat_install_test
runm hm_row_missing        $I "s/^    Module{\"hudmeter\", nullptr, \&HudmeterNormal, \"combat.hudmeter\", \&nevr_quest::CombatConfig::hudmeter},\$/    Module{\"hudmeter\", nullptr, \&HudmeterNormal},/" quest-host-combat combat_install_test
# the key: read, defaulted on, known to the schema
runm hm_key_default_off     src/quest/sentinel/quest_config.h "s/CombatSwitch hudmeter{true, Source::kAbsent}/CombatSwitch hudmeter{false, Source::kAbsent}/" quest-host-shared quest_config_test
runm hm_key_typo            src/quest/sentinel/quest_config.cpp "s/\"combat.hudmeter\"/\"combat.hudmeters\"/" quest-host-shared quest_config_test
runm hm_key_unread          src/quest/sentinel/quest_config.cpp "s/^  r.config.combat.hudmeter = ReadCombatSwitch(cfg, kCombatHudmeterKey, r);\$//" quest-host-shared quest_config_test
