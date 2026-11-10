# a mutant that leaves a variable, a parameter or a function unused would not compile under -Werror (not a kill): those append $NOWARN
NOWARN='1i #pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wunused-but-set-parameter"'
# dronekill (part 3 of 6): each rule of src/quest/combat/dronekill.cpp / dronekill_core.h removed once; src/quest/tests/dronekill_test.cpp must fail on every one
runm dc_r3_dead src/quest/combat/dronekill_core.h "s/if (!g.number || !g.dead || nowMs - g.ms >= kSwallowMs) continue;/if (!g.number || nowMs - g.ms >= kSwallowMs) continue;/" quest-host-combat dronekill_test
runm dc_r3_live src/quest/combat/dronekill_core.h "s/g.due |= 1u << i;/g.due |= 0;/" quest-host-combat dronekill_test
runm dc_r3_dueMs src/quest/combat/dronekill_core.h "s/                    g.dueMs = nowMs;\n//;/^                    g.dueMs = nowMs;\$/d" quest-host-combat dronekill_test
runm dc_k_mask src/quest/combat/dronekill_core.h "s/(g.handle \& 0xffff) == (index \& 0xffff)/g.handle == index/" quest-host-combat dronekill_test
runm dc_k_mask_one src/quest/combat/dronekill_core.h "s/(g.handle \& 0xffff) == (index \& 0xffff)/(g.handle \& 0xffff) == index/" quest-host-combat dronekill_test
runm dc_k_live src/quest/combat/dronekill_core.h "s/if (g.number \&\& !g.dead \&\& (g.handle \& 0xffff) == (index \& 0xffff)/if (g.number \&\& (g.handle \& 0xffff) == (index \& 0xffff)/" quest-host-combat dronekill_test
runm dc_k_newest src/quest/combat/dronekill_core.h "s/ \&\& (!best || g.number > best->number)) best = \&g;\n    if (!best) return nullptr;/XX/;/Group\* Killed/,/^    }/s/(!best || g.number > best->number)/(!best || g.number < best->number)/" quest-host-combat dronekill_test
runm dc_k_due src/quest/combat/dronekill_core.h "s/        if (due \&\& best->n) {/        if (false) {/;$NOWARN" quest-host-combat dronekill_test
runm dc_k_due_all src/quest/combat/dronekill_core.h "s/best->due = best->n >= 32 ? ~0u : (1u << best->n) - 1;/best->due = best->n >= 32 ? ~0u : (1u << best->n) - 2;/" quest-host-combat dronekill_test
runm dc_k_due_ms src/quest/combat/dronekill_core.h "s/            best->dueMs = nowMs;/            best->dueMs = 0;/;$NOWARN" quest-host-combat dronekill_test
runm dc_take_expire src/quest/combat/dronekill_core.h "s/            if (nowMs - g.dueMs >= kDueMs) { expired += Bits(g.due); g.due = 0; continue; }/            if (false) { continue; }/;$NOWARN" quest-host-combat dronekill_test
runm dc_take_count src/quest/combat/dronekill_core.h "s/expired += Bits(g.due); g.due = 0;/expired += 0; g.due = 0;/" quest-host-combat dronekill_test
runm dc_take_clear src/quest/combat/dronekill_core.h "s/expired += Bits(g.due); g.due = 0; continue;/expired += Bits(g.due); continue;/" quest-host-combat dronekill_test
runm dc_done src/quest/combat/dronekill_core.h "s/g->due \&= ~(1u << idx);/g->due \&= ~0u;/;$NOWARN" quest-host-combat dronekill_test
runm dc_anydue src/quest/combat/dronekill_core.h "s/        for (const auto\& g : groups) if (g.number \&\& g.due) return true;/        for (const auto\& g : groups) if (g.number \&\& g.due \&\& false) return true;/" quest-host-combat dronekill_test
runm dc_space_clear src/quest/combat/dronekill_core.h "s/        if (spaceNow \&\& space \&\& spaceNow != space) { Clear(); ++spaceClears; }/        if (false) { Clear(); ++spaceClears; }/" quest-host-combat dronekill_test
runm dc_space_set src/quest/combat/dronekill_core.h "s/        if (spaceNow) space = spaceNow;//" quest-host-combat dronekill_test
runm dc_reuse_slot src/quest/combat/dronekill_core.h "s/if (x.number \&\& x.system == system \&\& x.handle == handle) { g = \&x; break; }   \/\/ C2/if (false) { g = \&x; break; }/" quest-host-combat dronekill_test
runm dc_oldest src/quest/combat/dronekill_core.h "s/for (auto\& x : groups) if (x.number < g->number) g = \&x; }   \/\/ the oldest/for (auto\& x : groups) if (x.number > g->number) g = \&x; }/" quest-host-combat dronekill_test
runm dc_pending_actor src/quest/combat/dronekill_core.h "s/if (!actor || !p.actor || p.actor == actor) { Add(\*g, p); ++joined; }/if (true) { Add(*g, p); ++joined; }/" quest-host-combat dronekill_test
runm dc_pending_keep src/quest/combat/dronekill_core.h "s/                else pending\[kept++\] = p;                       \/\/ another drone's, released next//" quest-host-combat dronekill_test
runm dc_pending_n src/quest/combat/dronekill_core.h "s/            pendingN = kept;/            pendingN = 0;/" quest-host-combat dronekill_test
runm dc_join_actor src/quest/combat/dronekill_core.h "s/        if (g \&\& (!actor || !g->actor || g->actor == actor)) {/        if (g) {/" quest-host-combat dronekill_test
runm dc_newest_actor src/quest/combat/dronekill_core.h "s/if (g.number \&\& !g.dead \&\& g.actor == actor \&\& (!best/if (g.number \&\& !g.dead \&\& (!best/" quest-host-combat dronekill_test
runm dc_newest_fall src/quest/combat/dronekill_core.h "s/        if (best) return best;//" quest-host-combat dronekill_test
runm dc_pending_drop src/quest/combat/dronekill_core.h "s/            ++pendingDropped;//" quest-host-combat dronekill_test
runm dc_pending_full src/quest/combat/dronekill_core.h "s/        if (pendingN == kPending) {   \/\/ drop the oldest/        if (false) {/" quest-host-combat dronekill_test
runm dc_add_dup src/quest/combat/dronekill_core.h "s/for (int i = 0; i < g.n; ++i) if (g.comps\[i\].system == c.system \&\& g.comps\[i\].handle == c.handle) return;//" quest-host-combat dronekill_test
runm dc_add_overflow src/quest/combat/dronekill_core.h "s/if (g.n < kGroupComps) g.comps\[g.n++\] = c; else ++g.overflow;/if (g.n < kGroupComps) g.comps[g.n++] = c;/" quest-host-combat dronekill_test
runm dc_spaceof src/quest/combat/dronekill_core.h "s/for (int i = 0; g.number \&\& i < g.n; ++i) if (g.comps\[i\].owner == owner \&\& g.comps\[i\].space) return g.comps\[i\].space;/;/;$NOWARN" quest-host-combat dronekill_test
runm dc_spaceof_owner src/quest/combat/dronekill_core.h "s/        if (!owner) return 0;/        (void)0;/" quest-host-combat dronekill_test
runm dc_spaceof_pend src/quest/combat/dronekill_core.h "s/        for (int i = 0; i < pendingN; ++i) if (pending\[i\].owner == owner \&\& pending\[i\].space) return pending\[i\].space;//" quest-host-combat dronekill_test
runm dc_livecomp_dead src/quest/combat/dronekill_core.h "s/            if (!g.number || g.dead) continue;/            if (!g.number) continue;/" quest-host-combat dronekill_test
runm dc_liveactor_zero src/quest/combat/dronekill_core.h "s/        if (!actor) return false;\n        for (const auto\& g : groups) if (g.number \&\& !g.dead/XX/;/bool LiveActor/,/^    }/s/        if (!actor) return false;/        (void)0;/" quest-host-combat dronekill_test
runm dc_liveactor_dead src/quest/combat/dronekill_core.h "s/if (g.number \&\& !g.dead \&\& g.actor == actor) return true;/if (g.number \&\& g.actor == actor) return true;/" quest-host-combat dronekill_test
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
runm dc_gun_shown_zero src/quest/combat/dronekill_core.h "s/    bool Shown(uint64_t actor, uint32_t\* shown) const {\n        if (!actor) return false;/XX/;/bool Shown/,/^    }/s/        if (!actor) return false;/        (void)0;/" quest-host-combat dronekill_test
runm dc_gun_full src/quest/combat/dronekill_core.h "s/        if (slot < 0) { memmove(g, g + 1, sizeof g - sizeof g\[0\]); slot = kGuns - 1; }/        if (slot < 0) { slot = 0; }/" quest-host-combat dronekill_test
runm dc_gun_reuse src/quest/combat/dronekill_core.h "s/for (int i = 0; i < kGuns \&\& slot < 0; ++i) if (g\[i\].actor == actor) slot = i;//" quest-host-combat dronekill_test
runm dc_sym_b src/quest/combat/dronekill_core.h "s/kGun = 0x72F91FFFA17C3C8Cull,/kGun = 0x72F91FFFA17C3C8Dull,/;s/kPort = 0x45B09F0FA9F7FA33ull;/kPort = 0x45B09F0FA9F7FA34ull;/;s/kSatellite = 0x106C63A2927B7A4Dull,/kSatellite = 0x106C63A2927B7A4Cull,/;s/kNcaModel = 0x38EE951A26FB816Aull,/kNcaModel = 0x38EE951A26FB816Bull,/;s/kNetPhysics = 0x8FEE6557F995E4A3ull;/kNetPhysics = 0x8FEE6557F995E4A4ull;/" quest-host-combat dronekill_test
runm dc_chase_k src/quest/combat/dronekill_core.h "s/kChaseSpeed = 7.0f,/kChaseSpeed = 8.0f,/;s/kChaseDistance = 7.0f,/kChaseDistance = 6.0f,/;s/kChaseGain = 2.0f,/kChaseGain = 3.0f,/;s/kChaseAbove = 0.5f;/kChaseAbove = 0.6f;/" quest-host-combat dronekill_test
runm dc_chase_clamp src/quest/combat/dronekill_core.h "s/    speed = speed > kChaseSpeed ? kChaseSpeed : speed < -kChaseSpeed ? -kChaseSpeed : speed;//" quest-host-combat dronekill_test
runm dc_chase_clamp_lo src/quest/combat/dronekill_core.h "s/ : speed < -kChaseSpeed ? -kChaseSpeed : speed;/ : speed;/" quest-host-combat dronekill_test
runm dc_chase_clamp_hi src/quest/combat/dronekill_core.h "s/speed = speed > kChaseSpeed ? kChaseSpeed : speed < -kChaseSpeed/speed = speed < -kChaseSpeed/" quest-host-combat dronekill_test
runm dc_chase_zero src/quest/combat/dronekill_core.h "s/    if (dist2 < 1e-6f) return false;//" quest-host-combat dronekill_test
