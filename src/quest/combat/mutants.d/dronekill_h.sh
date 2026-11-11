# dronekill (part 8 of 8): the explosion's hold as its own switch, off by default, and what a hold needs (#1737 review of 17b174823):
# each rule removed once; src/quest/tests/dronekill_test.cpp (TestExplosionHoldOff, TestExplosionDeadEndsWhileAnotherLive,
# TestExplosionHoldNeedsOff) must fail on every one
runm dk_h_key src/quest/combat/dronekill.cpp "s/  g_hold = words.hold \&\& words.explosion;/  g_hold = words.explosion;/" quest-host-combat dronekill_test
runm dk_h_needsfx src/quest/combat/dronekill.cpp "s/  g_hold = words.hold \&\& words.explosion;/  g_hold = words.hold;/" quest-host-combat dronekill_test
runm dk_h_gate src/quest/combat/dronekill.cpp "s/  if (g_hold \&\& !d.enable \&\& d.system == kNcaPhysics \&\& TryHold(d)) return;/  if (g_explosion \&\& !d.enable \&\& d.system == kNcaPhysics \&\& TryHold(d)) return;/" quest-host-combat dronekill_test
runm dk_h_liveonly src/quest/combat/dronekill_core.h "s/            if (!g.used || g.x.ended || (g.dead \&\& nowMs - g.opened > kSwallowMs)) continue;/            if (!g.used || g.dead || nowMs == 0) continue;/" quest-host-combat dronekill_test
runm dk_h_window src/quest/combat/dronekill_core.h "s/            if (!g.used || g.x.ended || (g.dead \&\& nowMs - g.opened > kSwallowMs)) continue;/            if (!g.used || g.x.ended || (g.dead \&\& nowMs - g.opened > ~0ull)) continue;/" quest-host-combat dronekill_test
runm dk_h_deadone src/quest/combat/dronekill_core.h "s/        return n == 1 \&\& !one->dead ? one : nullptr;/        return n == 1 ? one : nullptr;/" quest-host-combat dronekill_test
runm dk_h_deadwhy src/quest/combat/dronekill_core.h "s/        \*dead = n == 1 \&\& one->dead;/        *dead = false;/" quest-host-combat dronekill_test
runm dk_h_ended src/quest/combat/dronekill.cpp "s/  g.x.ended = true;  \/\/ its end script ran/  (void)0;  \/\/ its end script ran/" quest-host-combat dronekill_test
runm dk_h_disshape src/quest/combat/dronekill.cpp "s/  if (DisableShape(system, \&disable) == 0) {/  if (DisableShape(system, \&disable) < 0) {/" quest-host-combat dronekill_test
runm dk_h_owner src/quest/combat/dronekill.cpp "s/  if (!SafeRead(reinterpret_cast<std::uintptr_t>(system) + kSysOwner, \&owner, sizeof owner) || owner == 0 ||/  if (false \&\&/" quest-host-combat dronekill_test
runm dk_h_openend src/quest/combat/dronekill.cpp "s/    const int opened = g_table.OpenEnd(cap.actor);/    const int opened = 0;/" quest-host-combat dronekill_test
runm dk_h_onlyone src/quest/combat/dronekill_core.h "s/        if (n != 1) return 0;/        if (n == 0) return 0;/" quest-host-combat dronekill_test
runm dk_h_endopen src/quest/combat/dronekill.cpp "s/  const bool held = g.x.holdOpen, open = held || g.x.endOpen;/  const bool held = g.x.holdOpen, open = held;/" quest-host-combat dronekill_test
runm dk_h_noheld src/quest/combat/dronekill.cpp "s/held ? \&h : nullptr, held ? 1 : 0, now)/\&h, 1, now)/" quest-host-combat dronekill_test
runm dk_h_warn src/quest/combat/dronekill.cpp "s/  HoldLine(LogLevel::kWarn, \"stays_on\", index, group,/  HoldLine(LogLevel::kDebug, \"stays_on\", index, group,/" quest-host-combat dronekill_test
runm dk_h_tickwarn src/quest/combat/dronekill.cpp "s/    else HoldLine(LogLevel::kWarn, \"stays_on\", work\[i\].index/    else HoldLine(LogLevel::kDebug, \"released\", work[i].index/" quest-host-combat dronekill_test
runm dk_h_takeover src/quest/combat/dronekill.cpp "s/      else direct\[ndirect++\] = open\[i\];/      else (void)0;/" quest-host-combat dronekill_test
runm dk_h_direct src/quest/combat/dronekill.cpp "s/  if (o.direct) return HeldOffNow(/  if (false) return HeldOffNow(/" quest-host-combat dronekill_test
runm dk_h_dropped src/quest/combat/dronekill.cpp "s/    if (dropped != 0) {/    if (false) {/" quest-host-combat dronekill_test
runm dk_h_alltakeover src/quest/combat/dronekill.cpp "s/      if (g.used \&\& g.x.holdOpen) open\[nopen++\] = FxHeld{g.x.held, g.x.number, g.handle};/      if (d.system == kDamageable \&\& g.used \&\& g.x.holdOpen) open[nopen++] = FxHeld{g.x.held, g.x.number, g.handle};/" quest-host-combat dronekill_test
