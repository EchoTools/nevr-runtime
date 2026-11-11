# a mutant that leaves a variable, a parameter or a function unused would not compile under -Werror (not a kill): those append $NOWARN
NOWARN='1i #pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wunused-but-set-parameter"'
# dronekill (part 3 of 6): each rule of src/quest/combat/dronekill.cpp / dronekill_core.h removed once; src/quest/tests/dronekill_test.cpp must fail on every one
# a row that names a drone kill rule (the group table, the gun table, the symbols, the chase velocity, the sizes and windows) mutates src/runtime/patch/dronekill_rules.h (#2359); dronekill_test compiles it through dronekill_core.h
runm dc_r3_dead src/runtime/patch/dronekill_rules.h "s/if (!g\[i\].used || !g\[i\].dead || now/if (!g[i].used || now/" quest-host-combat dronekill_test
runm dc_r3_live src/quest/combat/dronekill_core.h "s/g.x.due |= 1u << r.comp;/g.x.due |= 0;/" quest-host-combat dronekill_test
runm dc_r3_dueMs src/quest/combat/dronekill_core.h "/^            g.x.dueMs = nowMs;\$/d" quest-host-combat dronekill_test
runm dc_k_mask src/runtime/patch/dronekill_rules.h "s/(g\[i\].handle \& 0xffff) == (index \& 0xffff)/g[i].handle == index/" quest-host-combat dronekill_test
runm dc_k_mask_one src/runtime/patch/dronekill_rules.h "s/(g\[i\].handle \& 0xffff) == (index \& 0xffff)/(g[i].handle \& 0xffff) == index/" quest-host-combat dronekill_test
runm dc_k_live src/runtime/patch/dronekill_rules.h "s/if (g\[i\].used \&\& !g\[i\].dead \&\& (g\[i\].handle/if (g[i].used \&\& (g[i].handle/" quest-host-combat dronekill_test
# D9 (#2359): a kill picks the first live group in table order; the mutant picks the last
runm dc_k_newest src/runtime/patch/dronekill_rules.h "/int KillIndex/,/^    }/s/for (int i = 0; i < kGroups; ++i)/for (int i = kGroups - 1; i >= 0; --i)/" quest-host-combat dronekill_test
runm dc_k_due src/quest/combat/dronekill_core.h "s/        if (due \&\& g.n) {/        if (false) {/;$NOWARN" quest-host-combat dronekill_test
runm dc_k_due_all src/quest/combat/dronekill_core.h "s/g.x.due = g.n >= 32 ? ~0u : (1u << g.n) - 1;/g.x.due = g.n >= 32 ? ~0u : (1u << g.n) - 2;/" quest-host-combat dronekill_test
runm dc_k_due_ms src/quest/combat/dronekill_core.h "s/g.x.dueMs = nowMs; }/g.x.dueMs = 0; }/;$NOWARN" quest-host-combat dronekill_test
runm dc_take_expire src/quest/combat/dronekill_core.h "s/            if (nowMs - g.x.dueMs >= kDueMs) { expired += Bits(g.x.due); g.x.due = 0; continue; }/            if (false) { continue; }/;$NOWARN" quest-host-combat dronekill_test
runm dc_take_count src/quest/combat/dronekill_core.h "s/expired += Bits(g.x.due); g.x.due = 0;/expired += 0; g.x.due = 0;/" quest-host-combat dronekill_test
runm dc_take_clear src/quest/combat/dronekill_core.h "s/expired += Bits(g.x.due); g.x.due = 0; continue;/expired += Bits(g.x.due); continue;/" quest-host-combat dronekill_test
runm dc_done src/quest/combat/dronekill_core.h "s/g->x.due \&= ~(1u << idx);/g->x.due \&= ~0u;/;$NOWARN" quest-host-combat dronekill_test
runm dc_anydue src/quest/combat/dronekill_core.h "s/for (const auto\& g : t.g) if (g.used \&\& g.x.due) return true;/for (const auto\& g : t.g) if (g.used \&\& g.x.due \&\& false) return true;/" quest-host-combat dronekill_test
runm dc_space_clear src/runtime/patch/dronekill_rules.h "s/        if (spaceNow \&\& space \&\& spaceNow != space) { Clear(); ++spaceClears; cleared = true; }/        if (false) { Clear(); ++spaceClears; cleared = true; }/" quest-host-combat dronekill_test
runm dc_space_set src/runtime/patch/dronekill_rules.h "s/        if (spaceNow) space = spaceNow;//" quest-host-combat dronekill_test
runm dc_reuse_slot src/runtime/patch/dronekill_rules.h "s/        int s = Find(system, handle);/        int s = -1;/" quest-host-combat dronekill_test
# the release's slot pick (the one released first; the first in table order on a tie) is the header's; the mutant picks the one released last
runm dc_oldest src/runtime/patch/dronekill_rules.h "s/if (g\[i\].opened < g\[s\].opened) s = i;/if (g[i].opened > g[s].opened) s = i;/" quest-host-combat dronekill_test
runm dc_pending_actor src/runtime/patch/dronekill_rules.h "s/if (!actor || !p.actor || p.actor == actor) {/if (true) {/" quest-host-combat dronekill_test
runm dc_pending_keep src/runtime/patch/dronekill_rules.h "s/                pending\[kept++\] = p;/                (void)kept;/" quest-host-combat dronekill_test
runm dc_pending_n src/runtime/patch/dronekill_rules.h "s/        pendingN = kept;/        pendingN = 0;/" quest-host-combat dronekill_test
runm dc_join_actor src/runtime/patch/dronekill_rules.h "s/if (s >= 0 \&\& (!actor || !g\[s\].actor || g\[s\].actor == actor)) {/if (s >= 0) {/" quest-host-combat dronekill_test
runm dc_newest_actor src/runtime/patch/dronekill_rules.h "s/^        if (actor)\$/        if (actor \&\& false)/" quest-host-combat dronekill_test
runm dc_newest_fall src/runtime/patch/dronekill_rules.h "s/g\[i\].used \&\& !g\[i\].dead \&\& g\[i\].actor == actor) return i;/g[i].used \&\& !g[i].dead \&\& g[i].actor == actor) break;/" quest-host-combat dronekill_test
runm dc_pending_drop src/runtime/patch/dronekill_rules.h "s/pendingDropped += static_cast<uint64_t>(pendingN);//" quest-host-combat dronekill_test
runm dc_pending_full src/runtime/patch/dronekill_rules.h "s/        if (pendingN == kPending) {   \/\/ D6: full, every pending component goes/        if (false) {/" quest-host-combat dronekill_test
runm dc_add_dup src/runtime/patch/dronekill_rules.h "s/for (int i = 0; i < gr.n; ++i) if (gr.comps\[i\].system == system \&\& gr.comps\[i\].handle == handle) return i;//" quest-host-combat dronekill_test
runm dc_add_overflow src/quest/combat/dronekill_core.h "/\.x\.overflow += r.full;/d" quest-host-combat dronekill_test
runm dc_spaceof src/quest/combat/dronekill_core.h "s/for (int i = 0; g.used \&\& i < g.n; ++i) if (g.comps\[i\].x.owner == owner \&\& g.comps\[i\].x.space) return g.comps\[i\].x.space;/;/;$NOWARN" quest-host-combat dronekill_test
runm dc_spaceof_owner src/quest/combat/dronekill_core.h "s/        if (!owner) return 0;/        (void)0;/" quest-host-combat dronekill_test
runm dc_spaceof_pend src/quest/combat/dronekill_core.h "s/        for (int i = 0; i < t.pendingN; ++i) if (t.pending\[i\].x.owner == owner \&\& t.pending\[i\].x.space) return t.pending\[i\].x.space;//" quest-host-combat dronekill_test
runm dc_livecomp_dead src/runtime/patch/dronekill_rules.h "/bool LiveComp/,/^    }/s/if (!x.used || x.dead) continue;/if (!x.used) continue;/" quest-host-combat dronekill_test
runm dc_liveactor_zero src/quest/combat/dronekill_core.h "s/        if (!actor) return false;\n        for (const auto\& g : groups) if (g.number \&\& !g.dead/XX/;/bool LiveActor/,/^    }/s/        if (!actor) return false;/        (void)0;/" quest-host-combat dronekill_test
runm dc_liveactor_dead src/quest/combat/dronekill_core.h "s/if (g.used \&\& !g.dead \&\& g.actor == actor) return true;/if (g.used \&\& g.actor == actor) return true;/" quest-host-combat dronekill_test
runm dc_lock_strict src/quest/combat/dronekill_core.h "s/nowMs - t\[i\].deployed < kLockMs/nowMs - t[i].deployed <= kLockMs/" quest-host-combat dronekill_test
runm dc_lock_held src/quest/combat/dronekill_core.h "s/t\[i\].state == kTouchHeld \&\& nowMs - t\[i\].deployed/nowMs - t[i].deployed/" quest-host-combat dronekill_test
runm dc_lock_oldest src/quest/combat/dronekill_core.h "s/t\[i\].deployed > t\[newest\].deployed/t[i].deployed < t[newest].deployed/" quest-host-combat dronekill_test
runm dc_lock_mine src/quest/combat/dronekill_core.h "s/            if (t\[i\].atHandle == atHandle) mine = i;/            if (false) mine = i;/" quest-host-combat dronekill_test
runm dc_lock_held2 src/quest/combat/dronekill_core.h "s/        if (mine < 0 || t\[mine\].state != kTouchHeld) return -1;/        if (mine < 0) return -1;/" quest-host-combat dronekill_test
runm dc_lock_dueMs src/quest/combat/dronekill_core.h "s/        t\[mine\].dueMs = nowMs;/        t[mine].dueMs = 0;/" quest-host-combat dronekill_test
runm dc_deployed_life src/quest/combat/dronekill_core.h "s/if (t\[i\].state \&\& t\[i\].c.system == c.system \&\& t\[i\].c.handle == c.handle) slot = i;/if (false) slot = i;/" quest-host-combat dronekill_test
runm dc_deployed_free src/quest/combat/dronekill_core.h "s/for (int i = 0; i < kTouches \&\& slot < 0; ++i) if (!t\[i\].state) slot = i;//" quest-host-combat dronekill_test
runm dc_deployed_old src/quest/combat/dronekill_core.h "s/if (t\[i\].deployed < t\[oldest\].deployed) oldest = i;/if (t[i].deployed > t[oldest].deployed) oldest = i;/" quest-host-combat dronekill_test
runm dc_touch_expire src/quest/combat/dronekill_core.h "s/            if (nowMs - t\[i\].dueMs >= kDueMs) { ++expired; t\[i\].state = kTouchDone; continue; }/            if (false) { continue; }/;$NOWARN" quest-host-combat dronekill_test
runm dc_touch_exp_cnt src/quest/combat/dronekill_core.h "s/{ ++expired; t\[i\].state = kTouchDone; continue; }/{ t[i].state = kTouchDone; continue; }/" quest-host-combat dronekill_test
runm dc_touch_exp_st src/quest/combat/dronekill_core.h "s/{ ++expired; t\[i\].state = kTouchDone; continue; }/{ ++expired; continue; }/" quest-host-combat dronekill_test
runm dc_reon_done src/quest/combat/dronekill_core.h "s/t\[slot\].state = ok ? kTouchHeld : kTouchDone;/t[slot].state = kTouchHeld;/;$NOWARN" quest-host-combat dronekill_test
runm dc_reon_guard src/quest/combat/dronekill_core.h "s/void ReOnDone(int slot, bool ok) { if (t\[slot\].state == kTouchReOn) /void ReOnDone(int slot, bool ok) { if (true) /" quest-host-combat dronekill_test
runm dc_off_guard src/quest/combat/dronekill_core.h "s/void OffDone(int slot) { if (t\[slot\].state == kTouchOffDue) /void OffDone(int slot) { if (true) /" quest-host-combat dronekill_test
runm dc_touch_anydue src/quest/combat/dronekill_core.h "s/if (r.state == kTouchReOn || r.state == kTouchOffDue) return true;/if (r.state == kTouchReOn) return true;/" quest-host-combat dronekill_test
runm dc_gun_shown_zero src/runtime/patch/dronekill_rules.h "/bool Shown/,/^    }/s/        if (!key) return false; .*/        (void)0;/" quest-host-combat dronekill_test
runm dc_gun_full src/runtime/patch/dronekill_rules.h "s/            for (int i = 0; i + 1 < kGuns; ++i) e\[i\] = e\[i + 1\];//;s/            slot = kGuns - 1;/            slot = 0;/" quest-host-combat dronekill_test
runm dc_gun_reuse src/runtime/patch/dronekill_rules.h "s/for (int i = 0; i < kGuns \&\& slot < 0; ++i) if (e\[i\].key == key) slot = i;//" quest-host-combat dronekill_test
runm dc_sym_b src/runtime/patch/dronekill_rules.h "s/kGun = 0x72F91FFFA17C3C8Cull;/kGun = 0x72F91FFFA17C3C8Dull;/;s/kPort = 0x45B09F0FA9F7FA33ull;/kPort = 0x45B09F0FA9F7FA34ull;/;s/kSatellite = 0x106C63A2927B7A4Dull;/kSatellite = 0x106C63A2927B7A4Cull;/;s/kNcaModel = 0x38EE951A26FB816Aull;/kNcaModel = 0x38EE951A26FB816Bull;/" quest-host-combat dronekill_test
runm dc_sym_net src/quest/combat/dronekill_core.h "s/kNetPhysics = 0x8FEE6557F995E4A3ull;/kNetPhysics = 0x8FEE6557F995E4A4ull;/" quest-host-combat dronekill_test
runm dc_chase_k src/runtime/patch/dronekill_rules.h "s/kChaseSpeed = 7.0f;/kChaseSpeed = 8.0f;/;s/kChaseDistance = 7.0f;/kChaseDistance = 6.0f;/;s/kChaseGain = 2.0f;/kChaseGain = 3.0f;/;s/kChaseAbove = 0.5f;/kChaseAbove = 0.6f;/" quest-host-combat dronekill_test
runm dc_chase_clamp src/runtime/patch/dronekill_rules.h "s/    speed = speed > kChaseSpeed ? kChaseSpeed : speed < -kChaseSpeed ? -kChaseSpeed : speed;//" quest-host-combat dronekill_test
runm dc_chase_clamp_lo src/runtime/patch/dronekill_rules.h "s/ : speed < -kChaseSpeed ? -kChaseSpeed : speed;/ : speed;/" quest-host-combat dronekill_test
runm dc_chase_clamp_hi src/runtime/patch/dronekill_rules.h "s/speed = speed > kChaseSpeed ? kChaseSpeed : speed < -kChaseSpeed/speed = speed < -kChaseSpeed/" quest-host-combat dronekill_test
runm dc_chase_zero src/runtime/patch/dronekill_rules.h "s/    if (dist < kChaseMinDist) return false;//" quest-host-combat dronekill_test
