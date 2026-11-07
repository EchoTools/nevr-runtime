# gunconnect: each rule of src/quest/combat/gunconnect.cpp and its targets removed or bent once; src/quest/tests/gunconnect_test.cpp must fail on every one.
# Sourced by ../mutants.sh, which defines `runm <name> <file> <sed>`.
runm gc_no_hold src/quest/combat/gunconnect.cpp 's/if (now - st.since < kHoldS) continue;/;/'
runm gc_no_retry_gap src/quest/combat/gunconnect.cpp 's/if (now < st.nextTry) continue;/;/'
runm gc_no_max_tries src/quest/combat/gunconnect.cpp 's/if (st.tries >= kMaxTries) {/if (false) {/'
runm gc_gave_up_unset src/quest/combat/gunconnect.cpp 's/st.gaveUp = true;/;/'
runm gc_ring_ignored src/quest/combat/gunconnect.cpp 's/b = c.ok \&\& c.ring == 0;/b = true;/'
runm gc_no_tag_check src/quest/combat/gunconnect.cpp 's/ || !HasCombatTag(game, L.guid)/ || !(HasCombatTag(game, L.guid) || true)/'
runm gc_tag_zero_ok src/quest/combat/gunconnect.cpp 's/if (!ok || tag == 0) return false;/if (!ok) return false;/'
runm gc_tag_arg src/quest/combat/gunconnect.cpp 's/return g_f.hasTag(reinterpret_cast<void\*>(game), \&guid, tag);/return g_f.hasTag(reinterpret_cast<void*>(game), \&guid, tag + 1);/'
runm gc_state_only_3 src/quest/combat/gunconnect.cpp 's/if (SafeRead<std::uint32_t>(inst + gt::kInstanceState, \&ok2) != 0 \&\& ok2) ++c.unconnected;/if (SafeRead<std::uint32_t>(inst + gt::kInstanceState, \&ok2) == 3 \&\& ok2) ++c.unconnected;/'
runm gc_not_local_only src/quest/combat/gunconnect.cpp 's/!= slot || !ok2) continue;/!= slot \&\& !ok2) continue;/'
runm gc_flag_ignored src/quest/combat/gunconnect.cpp 's/if (SafeRead<std::uint32_t>(p + gt::kPropertyFlag, \&ok2) == 0 || !ok2) continue;/if (!ok2) continue;/'
runm gc_guns_unbounded src/quest/combat/gunconnect.cpp 's/if (!ok || c.guns > kMaxGuns) {/if (!ok) {/'
runm gc_hold_kept_unread src/quest/combat/gunconnect.cpp 's/for (FixState\& s : g_st) s.since = 0;/;/'
runm gc_hold_kept_notag src/quest/combat/gunconnect.cpp 's/^      st.since = 0;$//'
runm gc_hold_never_starts src/quest/combat/gunconnect.cpp 's/if (!st.since) st.since = now;/st.since = now;/'
runm gc_no_forget src/quest/combat/gunconnect.cpp 's/if (!live) s = FixState{};/;/'
runm gc_shared_state src/quest/combat/gunconnect.cpp 's/if (s.cs == cs) return s;/if (s.cs == cs || true) return g_st[0];/'
runm gc_wrong_cs src/quest/combat/gunconnect.cpp 's/g_f.connect(reinterpret_cast<void\*>(found\[k\].cs), L.userId);/g_f.connect(reinterpret_cast<void*>(found[0].cs), L.userId);/'
runm gc_idle_fast src/quest/combat/gunconnect.cpp 's/g_nextFix = now + kIdleEveryS;/g_nextFix = now + kFixEveryS;/'
runm gc_active_slow src/quest/combat/gunconnect.cpp 's/if (a || !c.ok) g_nextFix = std::min(g_nextFix, now + kFixEveryS);/;/'
runm gc_unread_slow src/quest/combat/gunconnect.cpp 's/if (a || !c.ok) g_nextFix/if (a) g_nextFix/'
runm gc_user_id src/quest/combat/gunconnect.cpp 's/L.userId = L.slot | L.gen << 16;/L.userId = L.slot;/'
runm gc_slot_unmasked src/quest/combat/gunconnect.cpp 's/ \& gt::kSlotMask;/;/'
runm gc_no_netgame src/quest/combat/gunconnect.cpp 's/if (!ok || L.net == 0) {/if (!ok) {/'
runm gc_no_de src/quest/combat/gunconnect.cpp 's/de == 0 || dc == 0/(de | 1) == 0 || dc == 0/'
runm gc_no_dc src/quest/combat/gunconnect.cpp 's/de == 0 || dc == 0/de == 0 || (dc | 1) == 0/'
runm gc_gen_unused src/quest/combat/gunconnect.cpp 's/if (L.gen == 0xffff || L.midx == 0xffff) {/if (L.midx == 0xffff) {/'
runm gc_midx_unused src/quest/combat/gunconnect.cpp 's/if (L.gen == 0xffff || L.midx == 0xffff) {/if (L.gen == 0xffff) {/'
runm gc_no_member_bound src/quest/combat/gunconnect.cpp 's/L.midx >= members/L.midx >= (members | 0xffffu)/'
runm gc_no_connected_log src/quest/combat/gunconnect.cpp 's/if (!a \&\& st.tries) {/if (false) {/'
runm gc_tries_kept src/quest/combat/gunconnect.cpp 's/^      st.tries = 0;$//'
runm gc_no_cs_log src/quest/combat/gunconnect.cpp 's/if (!st.seen) {/if (false) {/'
runm gc_hold_1s src/quest/combat/gunconnect.cpp 's/kHoldS = 2.0/kHoldS = 1.0/'
runm gc_retry_half src/quest/combat/gunconnect.cpp 's/kRetryS = 1.0/kRetryS = 0.5/'
runm gc_tries_9 src/quest/combat/gunconnect.cpp 's/kMaxTries = 10/kMaxTries = 9/'
runm gc_scope_always src/quest/combat/gunconnect.cpp 's/\&levelId) == level::Kind::kCombatOrLobby;/\&levelId) == level::Kind::kCombatOrLobby || true;/'
runm gc_scope_no_lobby src/quest/combat/gunconnect.cpp 's/\/\*lobby=\*\/true/\/*lobby=*\/false/'
runm gc_scope_inverted src/quest/combat/gunconnect.cpp 's/\&levelId) == level::Kind::kCombatOrLobby;/\&levelId) != level::Kind::kCombatOrLobby;/'
runm gc_scope_unseen src/quest/combat/gunconnect.cpp 's/g_scopedOut.fetch_add(1, std::memory_order_relaxed);/;/'
runm gc_off_ignored src/quest/combat/gunconnect.cpp 's/  if (!on) {/  if (!on \&\& false) {/'
runm gc_off_not_off src/quest/combat/gunconnect.cpp 's/return {false, "off_config", true};/return {false, "off_config", false};/'
runm gc_legacy_unwarned src/quest/combat/gunconnect.cpp 's/if (!legacy::Present("gunconnect.off")) return;/return;/'
runm gc_no_build_id src/quest/combat/gunconnect.cpp 's/if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", "");//'
runm gc_no_word_check src/quest/combat/gunconnect.cpp 's/  if (mismatch != nullptr) {/  if (false) {/'
runm gc_net_words_unchecked src/quest/combat/gunconnect.cpp 's/if (mismatch == nullptr) mismatch = pinned::FirstMismatch(image.base, netgame::kWords.*$//'
runm gc_name_mismatch_ok src/quest/combat/gunconnect.cpp 's/if (p != nullptr \&\& reinterpret_cast<std::uintptr_t>(p) != want) {/if (false) {/'
runm gc_export_elsewhere_ok src/quest/combat/gunconnect.cpp 's/if (!Resolve(image.base, symbols, \&fns, \&why)) return/if (false \&\& !Resolve(image.base, symbols, \&fns, \&why)) return/'
runm gc_tick_unregistered src/quest/combat/gunconnect.cpp 's/if (!tick::Add(\&Tick)) return Refused("tick_table_full", "");/tick::Add(\&Tick);/'
runm gc_quiet_refusal src/quest/combat/gunconnect.cpp 's/sentinel::LogLevel::kError, "combat_gunconnect_install"/sentinel::LogLevel::kInfo, "combat_gunconnect_install"/'
runm gc_addr_connect src/quest/combat/gunconnect_targets.h 's/kConnectVaddr = 0x2153438ULL/kConnectVaddr = 0x2153430ULL/'
runm gc_addr_hastag src/quest/combat/gunconnect_targets.h 's/kHasTagVaddr = 0x1723760ULL/kHasTagVaddr = 0x1723768ULL/'
runm gc_addr_useractor src/quest/combat/netgame_targets.h 's/kUserActorVaddr = 0x1251654ULL/kUserActorVaddr = 0x1251650ULL/'
runm gc_addr_tag src/quest/combat/gunconnect_targets.h 's/kTagCombatVaddr = 0x37af288ULL/kTagCombatVaddr = 0x37af280ULL/'
runm gc_gun_vptr src/quest/combat/gunconnect_targets.h 's/kGunCsVtableVaddr = 0x3696040ULL/kGunCsVtableVaddr = 0x3696048ULL/'
runm gc_gun_key src/quest/combat/gunconnect_targets.h 's/kGunCsKey = 0x7a804a0a9b2f5386ULL/kGunCsKey = 0x7a804a0a9b2f5387ULL/'
# The rows in combat_install.cpp (the shared file, judged by combat_install_test) and the gate's development-build flag.
I=src/quest/combat/combat_install.cpp
runm gc_row_off_counted_failed $I 's/    if (result.off) {/    if (false) {/' quest-host-combat combat_install_test
runm gc_row_off_uncounted $I 's/      ++outcome.off;/      ;/' quest-host-combat combat_install_test
runm gc_row_switch_record_on $I 's/{"on", sw.on ? 1 : 0}/{"on", 1}/' quest-host-combat combat_install_test
runm gc_row_gun_key $I 's/gunconnect::Install(g_config.gunconnect.on)/gunconnect::Install(true)/' quest-host-combat combat_install_test
runm gc_row_bots_key $I 's/switches.on = g_config.aibots.on;/switches.on = true;/' quest-host-combat combat_install_test
runm gc_row_arm_key $I 's/weapon_arm::Install(g_config.weaponArm.on)/weapon_arm::Install(true)/' quest-host-combat combat_install_test
runm gc_row_scenario_text $I 's/switches.scenario = g_config.aibotsScenario.source == nevr_quest::Source::kFile ? g_config.aibotsScenario.text : nullptr;/switches.scenario = nullptr;/' quest-host-combat combat_install_test
runm gc_row_dev_flag $I 's/switches.devBuild = DevBuild();/switches.devBuild = false;/' quest-host-combat combat_install_test
runm gc_row_order_arm_first $I 's/Module{"gunconnect", nullptr, \&GunconnectNormal, "combat.gunconnect", \&nevr_quest::CombatConfig::gunconnect},/Module{"gunconnect", nullptr, \&GunconnectNormal, "combat.aibots", \&nevr_quest::CombatConfig::gunconnect},/' quest-host-combat combat_install_test
runm gc_active_dev_unstored src/quest/combat/combat_active.cpp 's/  g_dev.store(devBuild ? 1 : 0, std::memory_order_release);/  ;/' quest-host-combat combat_install_test
# The keys (quest_config.cpp, nevr_config.cpp), judged by quest_config_test in the shared host group.
Q=src/quest/sentinel/quest_config.cpp
runm gc_key_default_off $Q 's/ReadCombatSwitch(cfg, kCombatGunconnectKey, r)/ReadCombatSwitch(cfg, kCombatGunconnectKey, r, \/*fallback=*\/false)/' quest-host-shared quest_config_test
runm gc_key_bots_default_off $Q 's/ReadCombatSwitch(cfg, kCombatAibotsKey, r)/ReadCombatSwitch(cfg, kCombatAibotsKey, r, \/*fallback=*\/false)/' quest-host-shared quest_config_test
runm gc_key_arm_default_off $Q 's/ReadCombatSwitch(cfg, kCombatWeaponArmKey, r)/ReadCombatSwitch(cfg, kCombatWeaponArmKey, r, \/*fallback=*\/false)/' quest-host-shared quest_config_test
runm gc_key_arm_misnamed $Q 's/kCombatWeaponArmKey = "combat.weapon_arm"/kCombatWeaponArmKey = "combat.weaponarm"/' quest-host-shared quest_config_test
runm gc_key_scenario_misnamed $Q 's/kCombatAibotsScenarioKey = "combat.aibots_scenario"/kCombatAibotsScenarioKey = "combat.aibots_scen"/' quest-host-shared quest_config_test
runm gc_key_scenario_unread $Q 's/r.config.combat.aibotsScenario = ReadCombatText(cfg, kCombatAibotsScenarioKey, r);/;/' quest-host-shared quest_config_test
runm gc_key_not_on_schema src/core/nevr_config.cpp 's/"gunconnect", "aibots", "aibots_scenario", "weapon_arm"}/"gunconnect", "aibots", "aibots_scenario"}/' quest-host-shared quest_config_test
G=src/quest/combat/gunconnect.cpp
# By count (the arena test counts SafeRead calls and level scans): a read, or a scan, made before the scope test.
runm gc_read_before_scope $G 's/^  if (!InScope(game)) {$/  (void)ReadLocal(game);\n  if (!InScope(game)) {/'
runm gc_scan_before_scope $G 's/^  if (!InScope(game)) {$/  (void)level::Scan(game);\n  if (!InScope(game)) {/'
runm gc_pins_connect_unchecked $G 's/const pinned::Pins lists\[\] = {gt::kPins, gt::kConnectPins, netgame::kPins, gamespace::targets::kPins};/const pinned::Pins lists[] = {gt::kPins, netgame::kPins, gamespace::targets::kPins};/'
runm gc_pins_gamespace_unchecked $G 's/const pinned::Pins lists\[\] = {gt::kPins, gt::kConnectPins, netgame::kPins, gamespace::targets::kPins};/const pinned::Pins lists[] = {gt::kPins, gt::kConnectPins, netgame::kPins};/'
runm gc_ring_u32 $G 's/c.ring = SafeRead<std::uint64_t>(cs + gt::kConnectRing, \&ok);/c.ring = SafeRead<std::uint32_t>(cs + gt::kConnectRing, \&ok);/'
