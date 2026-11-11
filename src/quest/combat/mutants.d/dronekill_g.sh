# dronekill (part 7 of 7): the explosion (#1737, dronekill.h "The explosion") and its shared queue rules (runtime/patch/dronekill_rules.h),
# each rule removed once; src/quest/tests/dronekill_test.cpp (TestExplosion) must fail on every one
runm dk_fx_hold2499 src/runtime/patch/dronekill_rules.h "s/constexpr uint64_t kFxSoundHoldMs = 2500;/constexpr uint64_t kFxSoundHoldMs = 2499;/" quest-host-combat dronekill_test
runm dk_fx_slots7 src/runtime/patch/dronekill_rules.h "s/constexpr int kFxSlots = 8; /constexpr int kFxSlots = 7; /" quest-host-combat dronekill_test
runm dk_fx_alive src/quest/combat/dronekill.cpp "s/fx = ExpiryLocked(reinterpret_cast<std::uint64_t>(system), handle, dead != 0, NowMs());/fx = ExpiryLocked(reinterpret_cast<std::uint64_t>(system), handle, dead == 0, NowMs());/" quest-host-combat dronekill_test
runm dk_fx_restore src/quest/combat/dronekill.cpp "s/  \*state = saved;  \/\/ after both/  (void)saved;/" quest-host-combat dronekill_test
runm dk_fx_zero_spawn src/quest/combat/dronekill.cpp "s/  \*state = 0;  \/\/ before the spawn entry/  (void)0;/" quest-host-combat dronekill_test
runm dk_fx_zero_sound src/quest/combat/dronekill.cpp "s/  \*state = 0;  \/\/ before the sound entry/  (void)0;/" quest-host-combat dronekill_test
runm dk_fx_lib src/quest/combat/dronekill_core.h "s/    return strcmp(slash ? slash + 1 : fname, lib) == 0;/    return lib != nullptr || slash != nullptr;/" quest-host-combat dronekill_test
runm dk_fx_offset src/quest/combat/dronekill_core.h "s/    if (!fn || !fname || !fbase || fn < fbase || fn - fbase != offset) return false;/    if (!fn || !fname || !fbase || fn < fbase || fn - fbase > offset + 0x100000) return false;/" quest-host-combat dronekill_test
runm dk_fx_secondk src/runtime/patch/dronekill_rules.h "s/            if (g\[i\].used \&\& !g\[i\].dead \&\& (g\[i\].handle \& 0xffff) == (index \& 0xffff)) {/            if (g[i].used \&\& (g[i].handle \& 0xffff) == (index \& 0xffff)) {/" quest-host-combat dronekill_test
runm dk_fx_letgo src/quest/combat/dronekill.cpp "s/  const int nlet = g_table.LetGo(now, let, letN, letI, kGroups);/  const int nlet = 0;/" quest-host-combat dronekill_test
runm dk_fx_realenter src/quest/combat/dronekill.cpp "s/d.system == kNcaPhysics \&\& TryHold(d)) return;/d.system == kNcaPhysics \&\& TryHold(d)) {}/" quest-host-combat dronekill_test
runm dk_fx_key src/quest/combat/dronekill.cpp "s/  g_explosion = words.explosion;/  g_explosion = true;/" quest-host-combat dronekill_test
runm dk_fx_sole src/quest/combat/dronekill_core.h "s/        return n == 1 ? one : nullptr;/        return n >= 1 ? one : nullptr;/" quest-host-combat dronekill_test
runm dk_fx_ctx src/quest/combat/dronekill.cpp "s/      if (Group\* g = g_table.ByNumber(j.group)) g->x.ctx = page_ctx;/      (void)page_ctx;/" quest-host-combat dronekill_test
runm dk_fx_killq src/quest/combat/dronekill.cpp "s/      if (g_explosion) fx = KilledLocked(\*g, NowMs());/      if (false) fx = KilledLocked(*g, NowMs());/" quest-host-combat dronekill_test
runm dk_fx_killrel src/quest/combat/dronekill.cpp "s/  if (g.x.holdOpen) ReleaseLocked(g, \"killed\", now, \&out);/  (void)0;/" quest-host-combat dronekill_test
runm dk_fx_full src/quest/combat/dronekill.cpp "s/      ReleaseLocked(g, \"queue_full\", now, \&out);/      g.x.holdOpen = false;/" quest-host-combat dronekill_test
runm dk_fx_defer src/quest/combat/dronekill.cpp "s/    if (k == 2 \&\& now - work\[i\].since < kDueMs) {/    if (false) {/" quest-host-combat dronekill_test
runm dk_fx_giveup src/quest/combat/dronekill.cpp "s/    if (k == 2 \&\& now - work\[i\].since < kDueMs) {/    if (k == 2) {/" quest-host-combat dronekill_test
runm dk_fx_count src/quest/combat/dronekill.cpp "s/  g_explosions.fetch_add(1, std::memory_order_relaxed);/  (void)0;/" quest-host-combat dronekill_test
runm dk_fx_heldcount src/quest/combat/dronekill.cpp "s/  g_held.fetch_add(1, std::memory_order_relaxed);/  (void)0;/" quest-host-combat dronekill_test
runm dk_fx_tick src/quest/combat/dronekill.cpp "s/  if (g_explosion) FxTick(now);/  if (false) FxTick(now);/" quest-host-combat dronekill_test
runm dk_fx_due src/quest/combat/dronekill.cpp "s/ \&\& !g_fx.Any() \&\& !g_offs.Any() \&\& !g_table.AnyHold())/)/" quest-host-combat dronekill_test
runm dk_fx_scopeclear src/quest/combat/dronekill.cpp "s/    g_fx.Clear();    \/\/ the level is gone/    \/\/ the level is gone/" quest-host-combat dronekill_test
runm dk_fx_reuse src/quest/combat/dronekill.cpp "s/        if ((g == nullptr || !g->x.holdOpen) \&\& g_offs.Add(open\[i\].c, open\[i\].group, open\[i\].index, NowMs()))/        if (false \&\& g_offs.Add(open[i].c, open[i].group, open[i].index, NowMs()) \&\& g == nullptr)/" quest-host-combat dronekill_test
