# a mutant that leaves a variable, a parameter or a function unused would not compile under -Werror (not a kill): those append $NOWARN
NOWARN='1i #pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wunused-but-set-parameter"'
# dronekill (part 2 of 6): each rule of src/quest/combat/dronekill.cpp / dronekill_core.h removed once; src/quest/tests/dronekill_test.cpp must fail on every one
# a row that names a drone kill rule (the group table, the gun table, the symbols, the chase velocity, the sizes and windows) mutates src/runtime/patch/dronekill_rules.h (#2359); dronekill_test compiles it through dronekill_core.h
runm dk_lock_never src/quest/combat/dronekill.cpp "s/  if (g_lockon) TouchTick(now, tid);//;$NOWARN" quest-host-combat dronekill_test
runm dk_touch_live src/quest/combat/dronekill.cpp "s/    const int k = live ? OffOne(r.c, \&why) : 1;/    const int k = OffOne(r.c, \&why);/" quest-host-combat dronekill_test
runm dk_touch_liveact src/quest/combat/dronekill.cpp "s/ || g_table.LiveActor(r.c.actor);/;/" quest-host-combat dronekill_test
runm dk_touch_livecomp src/quest/combat/dronekill.cpp "s/g_table.LiveComp(r.c.system, r.c.handle) || g_table.LiveActor/g_table.LiveActor/" quest-host-combat dronekill_test
runm dk_touch_reon_w src/quest/combat/dronekill.cpp "s/      if (k == 2) continue;\$/      if (false) continue;/" quest-host-combat dronekill_test
runm dk_touch_off_w src/quest/combat/dronekill.cpp "s/    if (k == 2) continue;\$/    if (false) continue;/" quest-host-combat dronekill_test
runm dk_touch_reon_ok src/quest/combat/dronekill.cpp "s/g_touch.ReOnDone(slots\[i\], k == 0);/g_touch.ReOnDone(slots[i], true);/" quest-host-combat dronekill_test
runm dk_off_no_revalid src/quest/combat/dronekill.cpp "s/  void\* const system = Revalidate(c, \&owner, why);\n  if (system == nullptr) return 1;\n  std::uintptr_t disable/XX/;/^int OffOne/,/^}/s/  if (system == nullptr) return 1;/  (void)0;/" quest-host-combat dronekill_test
runm dk_off_no_shape src/quest/combat/dronekill.cpp "s/  if (!DisableShape(system, \&disable)) {/  if (false) {/;$NOWARN" quest-host-combat dronekill_test
runm dk_off_no_flag src/quest/combat/dronekill.cpp "s/  if (flag == 0) return 2;  \/\/ Enter would queue here/  if (false) return 2;  \/\/ Enter would queue here/" quest-host-combat dronekill_test
runm dk_off_unread src/quest/combat/dronekill.cpp "s/  if (!SafeRead(owner + kOwnerFlag, \&flag, sizeof flag)) {\n    \*why = \"invoke-immediate word unreadable\";/XX/;/^int OffOne/,/^}/s/  if (!SafeRead(owner + kOwnerFlag, \&flag, sizeof flag)) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_off_category src/quest/combat/dronekill_core.h "s/kDisCategory = 1;/kDisCategory = 2;/" quest-host-combat dronekill_test
runm dk_on_no_shape src/quest/combat/dronekill.cpp "s/  if (!EnableShape(system, \&enable, \&at)) {/  if (false) {/;$NOWARN" quest-host-combat dronekill_test
runm dk_on_no_flag src/quest/combat/dronekill.cpp "/^int OnOne/,/^}/s/  if (flag == 0) return 2;/  (void)0;/" quest-host-combat dronekill_test
runm dk_on_no_bit src/quest/combat/dronekill.cpp "s/  if (bit == 0) return 2;  \/\/ the script's disable is still queued/  if (false) return 2;  \/\/ the script's disable is still queued/" quest-host-combat dronekill_test
runm dk_on_no_unread src/quest/combat/dronekill.cpp "s/  if (bit < 0) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_on_no_revalid src/quest/combat/dronekill.cpp "/^int OnOne/,/^}/s/  if (system == nullptr) return 1;/  (void)0;/" quest-host-combat dronekill_test
runm dk_bit_handle src/quest/combat/dronekill.cpp "s/  if (!SafeRead(idx + (handle \& 0xffff) \* 4ull, \&slot, sizeof slot)) return -1;/  if (!SafeRead(idx + handle * 4ull, \&slot, sizeof slot)) return -1;/" quest-host-combat dronekill_test
runm dk_bit_cat src/quest/combat/dronekill.cpp "s/  return mask \& 1;/  return mask \& 2;/" quest-host-combat dronekill_test
runm dk_bit_slot src/quest/combat/dronekill.cpp "s/  if (!SafeRead(masks + slot \* 2ull, \&mask, sizeof mask)) return -1;/  if (!SafeRead(masks + slot * 4ull, \&mask, sizeof mask)) return -1;/" quest-host-combat dronekill_test
runm dk_mask_idx src/quest/combat/dronekill_core.h "s/MaskAt{ Imm12(en\[3\]) \* 8, Imm12(en\[8\]) \* 8 }/MaskAt{ Imm12(en[8]) * 8, Imm12(en[3]) * 8 }/" quest-host-combat dronekill_test
runm dk_mask_scale src/quest/combat/dronekill_core.h "s/MaskAt{ Imm12(en\[3\]) \* 8,/MaskAt{ Imm12(en[3]) * 4,/" quest-host-combat dronekill_test
runm dk_revalid_space src/quest/combat/dronekill.cpp "s/  if (c.x.space == 0 || c.x.owner == 0) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_revalid_owner src/quest/combat/dronekill.cpp "s/ || owner != c.x.owner) {/) {/" quest-host-combat dronekill_test
runm dk_revalid_sys src/quest/combat/dronekill.cpp "s/  if (system != reinterpret_cast<void\*>(c.system)) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_revalid_shape src/quest/combat/dronekill.cpp "/^void\* Revalidate/,/^}/s/  if (!g_cmp_ok || !ComponentShape(system, \&component)) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_revalid_handle src/quest/combat/dronekill.cpp "s/  if (static_cast<std::uint32_t>(CallComponent(component, system, c.actor, c.x.compSym)) != c.handle) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_shape_cache src/quest/combat/dronekill.cpp "s/  if (hit) return cached;\n  std::uint32_t tw/XX/;/^int ComponentShape/,/^}/s/  if (hit) return cached;/  (void)cached;/" quest-host-combat dronekill_test
runm dk_shape_cache_d src/quest/combat/dronekill.cpp "/^int DisableShape/,/^}/s/  if (hit) return cached;/  (void)cached;/" quest-host-combat dronekill_test
runm dk_shape_cache_e src/quest/combat/dronekill.cpp "/^int EnableShape/,/^}/s/  if (hit) return cached;/  (void)cached;/" quest-host-combat dronekill_test
runm dk_cache_no_store src/quest/combat/dronekill.cpp "s/  CacheShape(g_vt, vt, ok);/  (void)0;/" quest-host-combat dronekill_test
runm dk_cache_ok_bit src/quest/combat/dronekill.cpp "s/vt | (ok ? 1 : 0), std::memory_order_acq_rel/vt | 1, std::memory_order_acq_rel/;$NOWARN" quest-host-combat dronekill_test
runm dk_cache_mask src/quest/combat/dronekill.cpp "s/    if ((v \& ~static_cast<std::uintptr_t>(1)) == vt) {/    if (v == vt) {/" quest-host-combat dronekill_test
runm dk_vt_odd src/quest/combat/dronekill.cpp "s/ \&\& \*vt != 0 \&\& (\*vt \& 1) == 0;/ \&\& *vt != 0;/" quest-host-combat dronekill_test
runm dk_shape_thunk src/quest/combat/dronekill.cpp "s/ \&\& ThunkOk(tw) \&\& BodyOk(bw, body, g_base + g_cmp_plt);/ \&\& BodyOk(bw, body, g_base + g_cmp_plt);/" quest-host-combat dronekill_test
runm dk_shape_body src/quest/combat/dronekill.cpp "s/ \&\& ThunkOk(tw) \&\& BodyOk(bw, body, g_base + g_cmp_plt);/ \&\& ThunkOk(tw);/" quest-host-combat dronekill_test
runm dk_shape_plt src/quest/combat/dronekill.cpp "s/BodyOk(bw, body, g_base + g_cmp_plt)/BodyOk(bw, body, body + kBodyBl * 4 + 4 * 0)/" quest-host-combat dronekill_test
runm dk_shape_dis src/quest/combat/dronekill.cpp "s/  const bool ok = SafeRead(body, w, sizeof w) \&\& DisableOk(w);/  const bool ok = SafeRead(body, w, sizeof w);/" quest-host-combat dronekill_test
runm dk_shape_en src/quest/combat/dronekill.cpp "s/  const bool ok = EnableOk(w);/  const bool ok = true; (void)w;/" quest-host-combat dronekill_test
runm dk_shape_comp_slot src/quest/combat/dronekill.cpp "s/!SafeRead(vt + kVtComponentId, \&body, sizeof body)/!SafeRead(vt + kVtComponent, \&body, sizeof body)/" quest-host-combat dronekill_test
runm dk_vt_slots src/quest/combat/dronekill_core.h "s/kVtComponentId = 0x1b0, kVtComponent = 0x1b8;/kVtComponentId = 0x1b8, kVtComponent = 0x1b0;/;s/kVtDisable = 0x210;/kVtDisable = 0x208;/;s/kVtEnable = 0x208;/kVtEnable = 0x210;/" quest-host-combat dronekill_test
runm dk_owner_k src/quest/combat/dronekill_core.h "s/kSysOwner = 0x80, kOwnerFlag = 0x368;/kSysOwner = 0x88, kOwnerFlag = 0x368;/;s/kSysOwner = 0x80, kOwnerFlag = 0x368;/kSysOwner = 0x80, kOwnerFlag = 0x36c;/" quest-host-combat dronekill_test
runm dc_words_a src/quest/combat/dronekill_core.h "s/0xf81f0ffe, 0xf9400008,/0xf81f0ffe, 0xf9400009,/;s/0xd10143ff, 0xa9025bf7,/0xd10143fe, 0xa9025bf7,/;s/0xa9bd5bf7, 0xa90153f5,/0xa9bd5bf6, 0xa90153f5,/;s/0xf81d0ff6, 0xa90153f5,/0xf81d0ff7, 0xa90153f5,/" quest-host-combat dronekill_test
runm dc_thunk_memcmp src/quest/combat/dronekill_core.h "s/inline bool ThunkOk(const uint32_t\* w) { return memcmp(w, kThunk, sizeof kThunk) == 0; }/inline bool ThunkOk(const uint32_t* w) { return memcmp(w, kThunk, 4) == 0; }/" quest-host-combat dronekill_test
runm dc_body_mask_bl src/quest/combat/dronekill_core.h "s/    if (i == kBodyBl) mask = ~kBlMask;/    if (false) mask = ~kBlMask;/" quest-host-combat dronekill_test
runm dc_body_bl_target src/quest/combat/dronekill_core.h "s/    return bodyVa + kBodyBl \* 4 + static_cast<uint64_t>(d \* 4) == pltVa;/    return true;/;$NOWARN" quest-host-combat dronekill_test
runm dc_body_bl_sign src/quest/combat/dronekill_core.h "s/    if (d \& 0x02000000) d -= 0x04000000;//" quest-host-combat dronekill_test
runm dc_body_ldr src/quest/combat/dronekill_core.h "s/    for (int k : kBodyLdr) if (i == k) mask = ~kLdrImmMask;/    for (int k : kBodyLdr) if (false \&\& i == k) mask = ~kLdrImmMask;/" quest-host-combat dronekill_test
runm dc_body_ldr_all src/quest/combat/dronekill_core.h "s/    for (int k : kBodyLdr) if (i == k) mask = ~kLdrImmMask;/    mask = ~kLdrImmMask;/" quest-host-combat dronekill_test
runm dc_dis_bl src/quest/combat/dronekill_core.h "s/    if (i == kDisBl) mask = ~kBlMask;/    if (false) mask = ~kBlMask;/" quest-host-combat dronekill_test
runm dc_dis_imm src/quest/combat/dronekill_core.h "s/    for (int k : kDisImm) if (i == k) mask = ~kLdrImmMask;/    for (int k : kDisImm) if (false \&\& i == k) mask = ~kLdrImmMask;/" quest-host-combat dronekill_test
runm dc_en_bl src/quest/combat/dronekill_core.h "s/    if (i == kEnBl) mask = ~kBlMask;/    if (false) mask = ~kBlMask;/" quest-host-combat dronekill_test
runm dc_en_imm src/quest/combat/dronekill_core.h "s/    for (int k : kEnImm) if (i == k) mask = ~kLdrImmMask;/    for (int k : kEnImm) if (false \&\& i == k) mask = ~kLdrImmMask;/" quest-host-combat dronekill_test
runm dc_en_last src/quest/combat/dronekill_core.h "s/0xf84307f6, 0xd65f03c0 };/0xf84307f6, 0xd65f03c1 };/" quest-host-combat dronekill_test
runm dc_times_a src/runtime/patch/dronekill_rules.h "s/kJoinMs = 5000;/kJoinMs = 5001;/;s/kPendingMs = 3000;/kPendingMs = 3001;/;s/kSwallowMs = 25000;/kSwallowMs = 25001;/" quest-host-combat dronekill_test
runm dc_join_strict src/runtime/patch/dronekill_rules.h "s/now - g\[i\].opened < kJoinMs/now - g[i].opened <= kJoinMs/" quest-host-combat dronekill_test
runm dc_pending_strict src/runtime/patch/dronekill_rules.h "s/if (now - p.at >= kPendingMs) continue;/if (now - p.at > kPendingMs) continue;/" quest-host-combat dronekill_test
# D8 (#2359): the swallow window includes kSwallowMs, so the boundary mutant narrows it (`>=`)
runm dc_swallow_strict src/runtime/patch/dronekill_rules.h "s/now - g\[i\].opened > kSwallowMs) continue;/now - g[i].opened >= kSwallowMs) continue;/" quest-host-combat dronekill_test
runm dc_times_b src/quest/combat/dronekill_core.h "s/kDueMs = 2000;/kDueMs = 2001;/;s/kLockMs = 120000;/kLockMs = 120001;/;s/kChaseMs = 100;/kChaseMs = 101;/" quest-host-combat dronekill_test
runm dc_due_strict src/quest/combat/dronekill_core.h "s/if (nowMs - g.x.dueMs >= kDueMs) { expired/if (nowMs - g.x.dueMs > kDueMs) { expired/" quest-host-combat dronekill_test
runm dc_group_size src/runtime/patch/dronekill_rules.h "s/kGroupComps = 20, kPending = 16;/kGroupComps = 20, kPending = 17;/" quest-host-combat dronekill_test
runm dc_no_r3 src/runtime/patch/dronekill_rules.h "s/if (!g\[i\].used || !g\[i\].dead || now - g\[i\].opened > kSwallowMs) continue;/continue;/;$NOWARN" quest-host-combat dronekill_test
