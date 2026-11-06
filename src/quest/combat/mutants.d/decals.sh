# decals: each rule of src/quest/combat/decals.cpp, decals_core.h and decals_targets.h removed or bent once; src/quest/tests/decals_test.cpp must fail on every
# one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target> [<command>]]`.
C=src/quest/combat/decals.cpp
K=src/quest/combat/decals_core.h
T=src/quest/combat/decals_targets.h
runm dm_no_finite_check $K "s/if (!FiniteBits(b + at + 4 \* k)) {/if (false) {/"
runm dm_data_cut_unchecked $K "s/if ((len - o - kGroupHeaderBytes) \/ kQuadBytes < q) return LoadResult::kGroupDataCut;//"
runm dm_quad_cap_unchecked $K "s/if (q > kMaxFileQuads) return LoadResult::kTooManyQuads;//"
runm dm_group_cap_unchecked $K "s/if (groups > kMaxGroups) return LoadResult::kTooManyGroups;//"
runm dm_magic_unchecked $K "s/if (std::memcmp(b, \"ADC1\", 4) != 0) return LoadResult::kBadMagic;//"
runm dm_no_node_cap $K "s/d.n = q > kMaxNodeQuads ? kMaxNodeQuads : q;/d.n = q;/"
runm dm_flip_u_noop       $K 's/  if (flip_u) {/  if (flip_u \&\& false) {/'
runm dm_flip_v_noop       $K 's/  if (flip_v) {/  if (flip_v \&\& false) {/'
runm dm_flip_u_one_pair $K "s/    swap_uv(3, 2);//"
runm dm_blend_any $K "s/return (b == 1 || b == 6 || b == 7 || b == 8 || b == 11) ? b : 1u;/return b == 0 ? 1u : b;/"
runm dm_blend_high_byte $K "s/const std::uint32_t b = flags \& 0xffu;/const std::uint32_t b = flags;/"
runm dm_no_war_room_rule $K "s/return group_level == level || (group_level == kLevelWarRoom \&\& level::IsCombatMap(level));/return group_level == level;/"
runm dm_war_room_anywhere $K "s/(group_level == kLevelWarRoom \&\& level::IsCombatMap(level))/(group_level == kLevelWarRoom)/"
runm dm_one_winding $K "s/t\[8\] = static_cast<std::uint16_t>(b + 1);/t[8] = static_cast<std::uint16_t>(b + 2);/"
runm dm_vertex_colour $K "s/out\[k\].c0 = out\[k\].c1 = 0xffffffffu;/out[k].c0 = out[k].c1 = 0;/"
runm dm_no_retry_gate $K "s/ \&\& now_ms >= ds->next) {/) {/"
runm dm_done_early         $K 's/ds->done = all || ds->tries >= kMaxTries;/ds->done = true || all;/'
runm dm_level_change_ignored $K "s/if (ds != nullptr \&\& ds->level != level) {/if (false) {/"
runm dm_no_war_room_hide $K "s/if (g.level == kLevelWarRoom \&\& level != kLevelWarRoom \&\& war_room_systems > 0) {/if (false) {/"
runm dm_war_room_hides_own $K "s/if (g.level == kLevelWarRoom \&\& level != kLevelWarRoom \&\& war_room_systems > 0) {/if (g.level == kLevelWarRoom \&\& war_room_systems > 0) {/"
runm dm_war_room_never_forgotten $K "s/if (s->level == kLevelWarRoom) --war_room_systems;//"
runm dm_shutdown_keeps_nodes $K "s/^    const int gone = DropNodes(sys);\$/    const int gone = 0;/"
runm dm_no_system_cap $K "s/if (sys_n >= kMaxSystems) {/if (false) {/"
runm dm_no_node_table_cap $K "s/if (node_n >= kMaxNodes) {/if (false) {/"
runm dm_window_never $K "s/if (now_ms - win_at >= kWindowMs) {/if (false) {/"
runm dm_blend_always      $C 's/  if (was == 0) {/  if (was == 0 || true) {/'
runm dm_blend_never       $C 's/  if (was == 0) {/  if (was == 0 \&\& false) {/'
runm dm_blend_offset $C "s/kNodeBlend = kNodeMesh + 0x320 + 0x700 + 8;/kNodeBlend = kNodeMesh + 0x320 + 0x700;/"
runm dm_no_graphics_gate $C "s/if (\*a.gs_initialized == 0 || \*a.mem_context == nullptr) return 0;/if (*a.mem_context == nullptr) return 0;/"
runm dm_index_count $C "s/(nodeAddr + kNodeIndexCount) = 12ull \* g.n;/(nodeAddr + kNodeIndexCount) = 6ull * g.n;/"
runm dm_vertex_count $C "s/kNodeVertexCount) = 4ull \* g.n;/kNodeVertexCount) = 3ull * g.n;/"
runm dm_not_shown $C "s/  SetVisible(nodeAddr, 1);/  (void)nodeAddr;/"
runm dm_unmap_skipped $C "s/  a.unmap(mesh);//"
runm dm_init_failure_kept $C "s/  if (\*rc != 0) {  \/\/ the node is destroyed and freed again/  if (false) {/"
runm dm_init_a1_wrong $C "s/ctx\[0\], ctx\[1\], g_idx, nullptr};/ctx[0], ctx[0], g_idx, nullptr};/"
runm dm_index_type $C "s/const MeshInit in{g.mat, 4ull \* g.n, 12ull \* g.n, 2, 1,/const MeshInit in{g.mat, 4ull * g.n, 12ull * g.n, 1, 1,/"
runm dm_node_size $C "s/kNodeBytes = 0x2048;/kNodeBytes = 0x2040;/"
runm dm_mesh_offset $C "s/kNodeMesh = 0xa8; /kNodeMesh = 0xa0; /"
runm dm_destroy_no_free $C "/^void OpDestroy/,/^}/ s/Vt<void (\*)(void\*, void\*)>(alloc, kVtFree)(alloc, node);//"
runm dm_destroy_no_dtor $C "/^void OpDestroy/,/^}/ s/Vt<void (\*)(void\*)>(node, kVtDestructor)(node);.*//"
runm dm_destroy_no_alloc $C "/^void OpDestroy/,/^}/ s/  a.mem_set(alloc);//"
runm dm_destroy_keeps_context $C '/^void OpDestroy/,/^}/ s/  a.mem_set(prev);/  (void)prev;/'
runm dm_update_real_skipped $C 's/^  original(sys);$/  (void)original;/'
runm dm_shutdown_real_skipped $C 's/^  return original(self);$/  (void)original; return self + 1;/'
runm dm_nodes_survive_shutdown $C "s/    g_host.Shutdown(static_cast<std::uintptr_t>(self));  .*/    (void)self;/"
runm dm_shutdown_not_first $C "s/  if (shutdownStatus != sentinel::GotStatus::kOk) {/  if (false) {/"
runm dm_empty_table_kept $C "s/if (lr != LoadResult::kOk || table.count == 0) {/if (lr != LoadResult::kOk) {/"
runm dm_missing_entry_ok $C "s/  } else if (!Complete(\*use)) {/  } else if (false) {/"
runm dm_search_data_dir   $C 's/  if (n < max \&\& legacy::Directory(dir, sizeof dir)) std::snprintf/  if (n < max \&\& false \&\& legacy::Directory(dir, sizeof dir)) std::snprintf/'
runm dm_update_pin $T 's/kUpdateRealVa = 0x2124290;/kUpdateRealVa = 0x2124294;/'
runm dm_update_slot_pin $T 's/kUpdateSlotVa = 0x370dbc8;/kUpdateSlotVa = 0x370dbc0;/'
runm dm_shutdown_slot_pin $T 's/kShutdownSlotVa = 0x36933e8;/kShutdownSlotVa = 0x36933e0;/'
runm dm_shutdown_real_pin $T 's/kShutdownRealVa = 0x21226f0;/kShutdownRealVa = 0x21226f4;/'
runm dm_war_room_symbol $K "s/kLevelWarRoom = 0x08a1af9e108def0bull;/kLevelWarRoom = 0x08a1af9e108def0cull;/"
runm dm_war_room_flag_not_counted $K "s/if (level == kLevelWarRoom) ++war_room_systems;/if (level == kLevelWarRoom) war_room_systems = 1;/"
# the scope: a game space whose level is not a combat map or the war room
runm dm_acts_everywhere      $K 's/return level::IsCombatMap(level) || level == kLevelWarRoom; }/return (level::IsCombatMap(level) || level == kLevelWarRoom) || true; }/'
runm dm_acts_no_war_room     $K 's/return level::IsCombatMap(level) || level == kLevelWarRoom; }/return level::IsCombatMap(level); }/'
runm dm_acts_lobby           $K 's/return level::IsCombatMap(level) || level == kLevelWarRoom; }/return level::IsCombatMap(level) || level == kLevelWarRoom || level::IsCombatLobby(level); }/'
runm dm_inert_never          $K 's/ds->inert = !ActsIn(level);/ds->inert = false;/'
runm dm_inert_no_return      $K 's/^      ds->recheck = now_ms + kInertRecheckMs;$/      ds->recheck = now_ms + kInertRecheckMs; ds->inert = false;/'
runm dm_inert_read_each      $K 's/if (ds != nullptr \&\& ds->inert \&\& now_ms < ds->recheck) return;//'
runm dm_inert_recheck_value  $K 's/kInertRecheckMs = 1000;/kInertRecheckMs = 100000;/'
runm dm_inert_never_rechecked $K 's/if (ds != nullptr \&\& ds->inert \&\& now_ms < ds->recheck) return;/if (ds != nullptr \&\& ds->inert) return;/'
runm dm_inert_recheck_unset  $K 's/^      ds->recheck = now_ms + kInertRecheckMs;$//'
runm dm_inert_line_acts      $K 's/{"acts", ds->inert ? 0 : 1}/{"acts", 1}/'
runm dm_war_room_applies     $K 's/(group_level == kLevelWarRoom \&\& level::IsCombatMap(level))/(group_level == kLevelWarRoom \&\& level::IsEchoArenaLevel(level))/'
# the install and the switches
runm dm_off_ignored          $C 's/^  if (!switches.on) {$/  if (false) {/'
runm dm_flip_u_ignored       $C 's/ParseTable(bytes, len, switches.flipU, switches.flipV, \&table)/ParseTable(bytes, len, false, switches.flipV, \&table)/'
runm dm_flip_v_ignored       $C 's/ParseTable(bytes, len, switches.flipU, switches.flipV, \&table)/ParseTable(bytes, len, switches.flipU, false, \&table)/'
runm dm_legacy_off_unwarned  $C 's/^  WarnLegacy("decals.off", "combat.decals: false");$//'
runm dm_legacy_flipu_unwarned $C 's/^  WarnLegacy("decalflipu.on", "combat.decals_flip_u: true");$//'
runm dm_legacy_flipv_unwarned $C 's/^  WarnLegacy("decalflipv.on", "combat.decals_flip_v: true");$//'
runm dm_already_installed_ok $C 's/^  if (g_on.load(std::memory_order_acquire))$/  if (false)/'
runm dm_no_table_is_error    $C 's/return {false, "no_table", true};/return {false, "no_table", false};/'
runm dm_off_config_is_error  $C 's/return {false, "off_config", true};/return {false, "off_config", false};/'
runm dm_no_lookup_ok         $C 's/if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, \&image)) {/if (false) {/'
runm dm_update_hook_not_armed $C 's/^  UpdateThunk::Arm(kUpdateRecord);$//'
runm dm_shutdown_hook_not_armed $C 's/^  ShutdownThunk::Arm(kShutdownRecord);$//'
runm dm_install_status       $C 's/return {false, sentinel::GotStatusName(updateStatus)};/return {true, "installed"};/'
runm dm_update_original_late $C 's/^  original(sys);$/  if (g_on.load(std::memory_order_acquire)) { pthread_mutex_lock(\&g_mu); g_host.Tick(sys, NowMs()); pthread_mutex_unlock(\&g_mu); }\n  original(sys);/'
# an arena match pays nothing, by count: a read before the scope test of the update handler is killed by the counters
runm dm_arena_read_in_update  $C 's/^  original(sys);$/  original(sys); { std::uintptr_t probe = 0; SafeRead(sys, \&probe, sizeof probe); }/'

# the quiet-space skip: an arena space, or one with no level yet, reaches the lock, the clock and the state machine once per skip, not on every update
runm dm_quick_skip_off         $C 's/^  if (QuickSkip(sys)) return;$/  if (false \&\& QuickSkip(sys)) return;/'
runm dm_quick_never_set        $C 's/QuickSet(sys, g_host.Quiet(sys));/QuickSet(sys, false);/'
runm dm_quick_always_quiet     $C 's/QuickSet(sys, g_host.Quiet(sys));/QuickSet(sys, true);/'
runm dm_quick_skip_forever     $C 's/g_quickSkip\[i\].store(n - 1, std::memory_order_relaxed);/(void)n;/'
runm dm_quiet_no_record        $K 's/return r == nullptr || r->inert;/return r != nullptr \&\& r->inert;/'
runm dm_quiet_never            $K 's/return r == nullptr || r->inert;/return false \&\& r == nullptr;/'
runm dm_quick_drop_skipped     $C 's/^    QuickDrop(static_cast<std::uintptr_t>(self));$//'
runm dm_flip_v_key_dropped     src/quest/combat/combat_install.cpp 's/"combat.decals_flip_v", \&nevr_quest::CombatConfig::decalsFlipV/nullptr, nullptr/' quest-host-combat combat_install_test
