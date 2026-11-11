# a mutant that leaves a variable, a parameter or a function unused would not compile under -Werror (not a kill): those append $NOWARN
NOWARN='1i #pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wunused-but-set-parameter"'
# a per-thread word shared by every thread (the mutants of the per-thread state)
SHARED_WORD='struct SharedWord { int Create() noexcept { return 0; } std::uintptr_t Get() const noexcept { return v; } void Set(std::uintptr_t x) noexcept { v = x; } std::uintptr_t v = 0; }'
# dronekill (part 4 of 7): each rule of src/quest/combat/dronekill.cpp / dronekill_core.h removed once; src/quest/tests/dronekill_test.cpp must fail on every one
# a row that names a drone kill rule (the group table, the gun table, the symbols, the chase velocity, the sizes and windows) mutates src/runtime/patch/dronekill_rules.h (#2359); dronekill_test compiles it through dronekill_core.h
runm dc_chase_dist_out src/runtime/patch/dronekill_rules.h "s/if (distOut) \*distOut = dist;/(void)distOut;/" quest-host-combat dronekill_test
runm dc_chase_above_y src/runtime/patch/dronekill_rules.h "s/q\[1\] + kChaseAbove - p\[1\]/q[1] - p[1]/" quest-host-combat dronekill_test
runm dc_chase_find src/quest/combat/dronekill_core.h "s/    Chase\* Find(uint64_t cs, uint32_t handle) {\n/XX/;/Chase\* Find/,/^    }/s/x.cs == cs \&\& x.handle == handle/x.cs == cs/;$NOWARN" quest-host-combat dronekill_test
runm dc_chase_take src/quest/combat/dronekill_core.h "s/if (!x.cs || (!x.moving \&\& !x.due)) {/if (!x.cs) {/" quest-host-combat dronekill_test
runm dc_chase_take_f src/quest/combat/dronekill_core.h "s/if (Chase\* x = Find(cs, handle)) return x;//" quest-host-combat dronekill_test
runm dc_chase_anydue src/quest/combat/dronekill_core.h "s/for (const auto\& x : c) if (x.due) return true;/for (const auto\& x : c) if (x.due \&\& false) return true;/" quest-host-combat dronekill_test
runm dc_at_a src/quest/combat/dronekill_core.h "s/kAtIdx = 0xc8, kAtRecs = 0xf8, kAtStride = 0x90,/kAtIdx = 0xc8, kAtRecs = 0xf8, kAtStride = 0x98,/;s/kAtTransforms = 0x130, kAtPhysics = 0x138;/kAtTransforms = 0x138, kAtPhysics = 0x130;/;s/kAtHasTarget = 0x63,/kAtHasTarget = 0x64,/;s/kAtTransform = 0x7c,/kAtTransform = 0x78,/" quest-host-combat dronekill_test
runm dc_at_b src/quest/combat/dronekill_core.h "s/kAtTargetIdx = 0x80,/kAtTargetIdx = 0x7c,/;s/kAtPhysIdx = 0x84,/kAtPhysIdx = 0x88,/;s/kAtTargetSys = 0x88;/kAtTargetSys = 0x90;/" quest-host-combat dronekill_test
runm dc_dm_a src/quest/combat/dronekill_core.h "s/kDmIdx = 0xd0, kDmRecs = 0x100, kDmStride = 0xb8, kDmPhysH = 0xc, kDmXfH = 0x8, kDmPhys = 0x140,/kDmIdx = 0xd8, kDmRecs = 0x100, kDmStride = 0xb8, kDmPhysH = 0xc, kDmXfH = 0x8, kDmPhys = 0x140,/;s/kDmRecs = 0x100,/kDmRecs = 0x108,/;s/kDmStride = 0xb8,/kDmStride = 0xc0,/" quest-host-combat dronekill_test
runm dc_dm_b src/quest/combat/dronekill_core.h "s/kDmPhysH = 0xc, kDmXfH = 0x8,/kDmPhysH = 0x8, kDmXfH = 0xc,/;s/kDmPhys = 0x140,/kDmPhys = 0x138,/;s/kDmXf = 0x138, kBodyPos = 0x8f8;/kDmXf = 0x140, kBodyPos = 0x8f8;/;s/kBodyPos = 0x8f8;/kBodyPos = 0x8f0;/" quest-host-combat dronekill_test
runm dc_ph src/quest/combat/dronekill_core.h "s/kNetPhysicsPhys = 0x170;/kNetPhysicsPhys = 0x178;/;s/kPhIdx = 0xc8, kPhOwners = 0x100;/kPhIdx = 0xd0, kPhOwners = 0x100;/;s/kPhIdx = 0xc8, kPhOwners = 0x100;/kPhIdx = 0xc8, kPhOwners = 0x108;/" quest-host-combat dronekill_test
runm dk_gun_word src/quest/combat/dronekill.cpp "s/  if (g_gun \&\& d.system == kNcaModel \&\& (d.component == kGun || d.component == kPort)) {/  if (d.system == kNcaModel \&\& (d.component == kGun || d.component == kPort)) {/" quest-host-combat dronekill_test
runm dk_gun_system src/quest/combat/dronekill.cpp "s/  if (g_gun \&\& d.system == kNcaModel \&\& (d.component == kGun || d.component == kPort)) {/  if (g_gun \&\& (d.component == kGun || d.component == kPort)) {/" quest-host-combat dronekill_test
runm dk_gun_comp src/quest/combat/dronekill.cpp "s/ \&\& (d.component == kGun || d.component == kPort)) {/ \&\& (d.component == kGun)) {/" quest-host-combat dronekill_test
runm dk_gun_set src/quest/combat/dronekill.cpp "s/    if (d.component == kGun) g_guns.Set(d.actor, d.enable);/    if (d.component == kGun) (void)0;/" quest-host-combat dronekill_test
runm dk_gun_shown src/quest/combat/dronekill.cpp "s/    else known = g_guns.Shown(d.actor, \&shown);/    else known = false;/" quest-host-combat dronekill_test
runm dk_gun_sat src/quest/combat/dronekill.cpp "s/for (const std::uint64_t sym : {kSatellite, kPort}) {/for (const std::uint64_t sym : {kPort}) {/" quest-host-combat dronekill_test
runm dk_gun_port src/quest/combat/dronekill.cpp "s/for (const std::uint64_t sym : {kSatellite, kPort}) {/for (const std::uint64_t sym : {kSatellite}) {/" quest-host-combat dronekill_test
runm dk_gun_m3 src/quest/combat/dronekill.cpp "s/        if ((static_cast<std::uint32_t>(CallComponent(component, system, cap.actor, sym)) \& 0xffff) == 0xffff) {  \/\/ M3/        if (false) {  \/\/ M3/" quest-host-combat dronekill_test
runm dk_gun_copy src/quest/combat/dronekill.cpp "s/        copy.component = sym;/        (void)sym;/" quest-host-combat dronekill_test
runm dk_gun_real src/quest/combat/dronekill.cpp "s/        real(node, \&copy);\n      }/XX/;/for (const std::uint64_t sym/,/^      }/s/        real(node, \&copy);/        (void)copy;/" quest-host-combat dronekill_test
runm dk_port_follow src/quest/combat/dronekill.cpp "s/    } else if (known \&\& shown != d.enable) {  \/\/ M2/    } else if (false) {  \/\/ M2/;$NOWARN" quest-host-combat dronekill_test
runm dk_port_known src/quest/combat/dronekill.cpp "s/    } else if (known \&\& shown != d.enable) {  \/\/ M2/    } else if (shown != d.enable) {  \/\/ M2/;$NOWARN" quest-host-combat dronekill_test
runm dk_port_same src/quest/combat/dronekill.cpp "s/    } else if (known \&\& shown != d.enable) {  \/\/ M2/    } else if (known) {  \/\/ M2/" quest-host-combat dronekill_test
runm dk_port_enable src/quest/combat/dronekill.cpp "s/      copy.enable = shown;//" quest-host-combat dronekill_test
runm dk_table_on src/quest/combat/dronekill.cpp "s/  if (d.enable) {\n    pthread_mutex_lock(\&g_mu);  \/\/ DK-5/XX/;/^  Joined j{0, 0, false, false};/,/^  if (j.again) {/s/  if (d.enable) {/  if (true) {/" quest-host-combat dronekill_test
runm dk_table_never src/quest/combat/dronekill.cpp "/^  Joined j{0, 0, false, false};/,/^  if (j.again) {/s/  if (d.enable) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_again_due src/quest/combat/dronekill.cpp "s/    if (j.again \&\& g_kill) g_any_due.store(true, std::memory_order_release);  \/\/ R3/    if (j.again) (void)0;  \/\/ R3/" quest-host-combat dronekill_test
runm dk_again_kill src/quest/combat/dronekill.cpp "s/    if (j.again \&\& g_kill) g_any_due/    if (j.again) g_any_due/" quest-host-combat dronekill_test
runm dk_again_return src/quest/combat/dronekill.cpp "/^  if (j.again) {/,/^    return;/s/^    return;\$/    (void)0;/" quest-host-combat dronekill_test
runm dk_caps src/quest/combat/dronekill.cpp "s/kLineCap = 160, kNoteCap = 24, kOffLineCap = 64, kKLineCap = 48, kF357LineCap = 96;/kLineCap = 160, kNoteCap = 24, kOffLineCap = 65, kKLineCap = 48, kF357LineCap = 96;/;s/kLineCap = 160,/kLineCap = 161,/;s/kNoteCap = 24,/kNoteCap = 25,/;s/kKLineCap = 48,/kKLineCap = 49,/;s/kF357LineCap = 96;/kF357LineCap = 97;/" quest-host-combat dronekill_test
runm dk_allow_cap src/quest/combat/dronekill.cpp "/^bool Allow/,/^}/s/  if (v == cap) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_allow_limit src/quest/combat/dronekill.cpp "s/  if (v < cap) return true;/  if (v <= cap) return true;/" quest-host-combat dronekill_test
runm dk_release_line src/quest/combat/dronekill.cpp "s/  if (d.enable \&\& d.system == kDamageable) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_release_only src/quest/combat/dronekill.cpp "s/  if (d.enable \&\& d.system == kDamageable) {/  if (d.system == kDamageable) {/" quest-host-combat dronekill_test
runm dk_where_pending src/quest/combat/dronekill.cpp "s/  else if (j.pending) std::snprintf(where, sizeof where, \"pending\");/  else if (false) std::snprintf(where, sizeof where, \"pending\");/" quest-host-combat dronekill_test
runm dk_fire_scope src/quest/combat/dronekill.cpp "s/  if (!g_inScope.load(std::memory_order_relaxed)) return real(cs, handle, velocity, remote);/  if (false) return real(cs, handle, velocity, remote);/" quest-host-combat dronekill_test
runm dk_fire_nocount src/quest/combat/dronekill.cpp "s/  g_fires.fetch_add(1, std::memory_order_relaxed);/  g_fires.fetch_add(0, std::memory_order_relaxed);/" quest-host-combat dronekill_test
runm dk_fire_vel src/quest/combat/dronekill.cpp "s/^  ShotFrame frame{velocity, remote};\$/  ShotFrame frame{DkC3{}, remote};/" quest-host-combat dronekill_test
runm dk_fire_remote src/quest/combat/dronekill.cpp "s/^  ShotFrame frame{velocity, remote};\$/  ShotFrame frame{velocity, 0};/" quest-host-combat dronekill_test
runm dk_fire_in src/quest/combat/dronekill.cpp "s/^  g_t_shot.Set(reinterpret_cast<std::uintptr_t>(\&frame));\$/  (void)frame;/" quest-host-combat dronekill_test
runm dk_fire_unset src/quest/combat/dronekill.cpp "s/^  g_t_shot.Set(outer);\$/  (void)outer;/" quest-host-combat dronekill_test
runm dk_fire_restore src/quest/combat/dronekill.cpp "s/^  g_t_shot.Set(outer);\$/  g_t_shot.Set(0);/;$NOWARN" quest-host-combat dronekill_test
runm dk_fire_args src/quest/combat/dronekill.cpp "s/  real(cs, handle, velocity, remote);/  real(cs, handle, DkC3{0, 0, 0}, remote);/" quest-host-combat dronekill_test
runm dk_fire_args_r src/quest/combat/dronekill.cpp "s/  real(cs, handle, velocity, remote);/  real(cs, handle, velocity, 0);/" quest-host-combat dronekill_test
runm dk_shot_outside src/quest/combat/dronekill.cpp "s/  if (frame == nullptr) return nullptr;/  (void)0;/" quest-host-combat dronekill_test
runm dk_shot_remote src/quest/combat/dronekill.cpp "s/  if (remote != nullptr) \*remote = static_cast<int>(frame->remote);/  (void)remote;/" quest-host-combat dronekill_test
runm dk_shot_active src/quest/combat/dronekill.cpp "s/bool ShotActive() noexcept { return g_shot_swapped.load(std::memory_order_acquire); }/bool ShotActive() noexcept { return true; }/" quest-host-combat dronekill_test
runm dk_atk_scope src/quest/combat/dronekill.cpp "/^void AttackHandler/,/^}/s/  if (!g_inScope.load(std::memory_order_relaxed)) return;/  (void)0;/" quest-host-combat dronekill_test
runm dk_atk_order src/quest/combat/dronekill.cpp "/^void AttackHandler/,/^}/{s/^  real(cs, handle);\$/  (void)0;/;s/^  AttackInScope(cs, handle);\$/  AttackInScope(cs, handle);\n  real(cs, handle);/}" quest-host-combat dronekill_test
runm dk_atk_count src/quest/combat/dronekill.cpp "s/  g_attacks.fetch_add(1, std::memory_order_relaxed);/  g_attacks.fetch_add(0, std::memory_order_relaxed);/" quest-host-combat dronekill_test
runm dk_chase_handle src/quest/combat/dronekill.cpp "s/  if (!SafeRead(cs + kAtIdx, \&idx, sizeof idx) || !idx || !SafeRead(idx + (handle \& 0xffff) \* 4ull, \&slot, sizeof slot)) return;/  if (!SafeRead(cs + kAtIdx, \&idx, sizeof idx) || !idx || !SafeRead(idx + handle * 4ull, \&slot, sizeof slot)) return;/" quest-host-combat dronekill_test
runm dk_chase_stride src/quest/combat/dronekill.cpp "s/  const std::uintptr_t rec = recs + slot \* kAtStride;/  const std::uintptr_t rec = recs + slot * 0x80;/" quest-host-combat dronekill_test
runm dk_chase_xforms src/quest/combat/dronekill.cpp "s/  if (!SafeRead(cs + kAtTransforms, \&transforms, sizeof transforms) || !transforms) return;/  if (!SafeRead(cs + kAtTransforms, \&transforms, sizeof transforms)) return;/" quest-host-combat dronekill_test
runm dk_chase_atowner src/quest/combat/dronekill.cpp "s/  if (!SafeRead(cs + kSysOwner, \&at_owner, sizeof at_owner) || !at_owner) return;/  if (!SafeRead(cs + kSysOwner, \&at_owner, sizeof at_owner)) return;/" quest-host-combat dronekill_test
runm dk_chase_has src/quest/combat/dronekill.cpp "s/^  if (has) {\$/  if (true) {/" quest-host-combat dronekill_test
runm dk_chase_nohas src/quest/combat/dronekill.cpp "s/^  if (has) {\$/  if (false) {/" quest-host-combat dronekill_test
runm dk_chase_drec src/quest/combat/dronekill.cpp "s/    const std::uintptr_t drec = d_recs + d_slot \* kDmStride;/    const std::uintptr_t drec = d_recs + d_slot * 0x80;/" quest-host-combat dronekill_test
runm dk_chase_body src/quest/combat/dronekill.cpp "s/    if (SafeRead(ts + kDmPhys, \&t_phys, sizeof t_phys) \&\& t_phys) {/    if (false) {/" quest-host-combat dronekill_test
runm dk_chase_fallback src/quest/combat/dronekill.cpp "s/    if (body == nullptr || !SafeRead(reinterpret_cast<std::uintptr_t>(body) + kBodyPos, q, sizeof q)) {  \/\/ AttackTarget's fallback/    if (false) {/;$NOWARN" quest-host-combat dronekill_test
runm dk_chase_fb_xf src/quest/combat/dronekill.cpp "s/      if (!SafeRead(ts + kDmXf, \&t_xf, sizeof t_xf) || !t_xf) return;/      if (!SafeRead(ts + kDmXf, \&t_xf, sizeof t_xf)) return;/" quest-host-combat dronekill_test
runm dk_shot_shared_word src/quest/combat/dronekill.cpp "s/^ThreadWord g_t_shot;.*/$SHARED_WORD g_t_shot;/" quest-host-combat dronekill_test
runm dk_seam_no_clear src/quest/combat/dronekill.cpp "s/^    g_t_shot.Set(0);\$/    (void)0;/" quest-host-combat dronekill_test
