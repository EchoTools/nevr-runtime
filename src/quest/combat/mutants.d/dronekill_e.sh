# a mutant that leaves a variable, a parameter or a function unused would not compile under -Werror (not a kill): those append $NOWARN
NOWARN='1i #pragma GCC diagnostic ignored "-Wunused-variable"\n#pragma GCC diagnostic ignored "-Wunused-parameter"\n#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n#pragma GCC diagnostic ignored "-Wunused-function"\n#pragma GCC diagnostic ignored "-Wunused-but-set-parameter"'
# a per-thread word shared by every thread (the mutants of the per-thread state)
SHARED_WORD='struct SharedWord { int Create() noexcept { return 0; } std::uintptr_t Get() const noexcept { return v; } void Set(std::uintptr_t x) noexcept { v = x; } std::uintptr_t v = 0; }'
# dronekill (part 5 of 6): each rule of src/quest/combat/dronekill.cpp / dronekill_core.h removed once; src/quest/tests/dronekill_test.cpp must fail on every one
runm dk_chase_fb_h src/quest/combat/dronekill.cpp "s/      const DkTransfQ them = g_fns.l2w(reinterpret_cast<void\*>(t_xf), xf_h);/      const DkTransfQ them = g_fns.l2w(reinterpret_cast<void*>(t_xf), body_h);/" quest-host-combat dronekill_test
runm dk_chase_me_h src/quest/combat/dronekill.cpp "s/    const DkTransfQ me = g_fns.l2w(reinterpret_cast<void\*>(transforms), xf);/    const DkTransfQ me = g_fns.l2w(reinterpret_cast<void*>(transforms), phys_h);/" quest-host-combat dronekill_test
runm dk_chase_primary src/quest/combat/dronekill.cpp "s/      const DkPhTarget t = g_fns.primary(reinterpret_cast<void\*>(t_phys), body_h);/      const DkPhTarget t = g_fns.primary(reinterpret_cast<void*>(t_phys), xf_h);/" quest-host-combat dronekill_test
runm dk_chase_zero src/quest/combat/dronekill.cpp "s/    if (!ChaseVelocity(me.p, q, v, \&dist)) return;  \/\/ on the target: EchoCombat writes nothing/    ChaseVelocity(me.p, q, v, \&dist);/" quest-host-combat dronekill_test
runm dk_chase_space src/quest/combat/dronekill.cpp "s/  space = reinterpret_cast<void\*>(g_table.SpaceOf(owner));  \/\/ DFC-1/  space = g_aux_space;  \/\/ DFC-1/" quest-host-combat dronekill_test
runm dk_chase_slot_f src/quest/combat/dronekill.cpp "s/  Chase\* c = moving ? g_chases.Take(cs, handle) : g_chases.Find(cs, handle);/  Chase* c = g_chases.Find(cs, handle);/" quest-host-combat dronekill_test
runm dk_chase_rest src/quest/combat/dronekill.cpp "s/  if (c == nullptr || (!moving \&\& !was_moving)) return;/  if (c == nullptr) return;/;$NOWARN" quest-host-combat dronekill_test
runm dk_chase_rest_n src/quest/combat/dronekill.cpp "s/  if (c == nullptr || (!moving \&\& !was_moving)) return;/  if (c == nullptr || !moving) return;/;$NOWARN" quest-host-combat dronekill_test
runm dk_chase_local src/quest/combat/dronekill.cpp "s/    c->due = local;/    c->due = true;/" quest-host-combat dronekill_test
runm dk_chase_local_no src/quest/combat/dronekill.cpp "s/    c->due = local;/    c->due = false;/" quest-host-combat dronekill_test
runm dk_chase_ms src/quest/combat/dronekill.cpp "s/    c->ms = NowMs();/    c->ms = 0;/" quest-host-combat dronekill_test
runm dk_chase_v src/quest/combat/dronekill.cpp "s/    std::memcpy(c->v, v, sizeof v);//" quest-host-combat dronekill_test
runm dk_chase_phys_s src/quest/combat/dronekill.cpp "s/    c->physics = physics;//" quest-host-combat dronekill_test
runm dk_chase_owner_s src/quest/combat/dronekill.cpp "s/    c->owner = owner;//" quest-host-combat dronekill_test
runm dk_chase_atowner_s src/quest/combat/dronekill.cpp "s/    c->atOwner = at_owner;//" quest-host-combat dronekill_test
runm dk_chase_phys_h src/quest/combat/dronekill.cpp "s/    c->phys = phys_h;//" quest-host-combat dronekill_test
runm dk_chase_moving src/quest/combat/dronekill.cpp "s/    c->moving = moving;//" quest-host-combat dronekill_test
runm dk_chase_anydue src/quest/combat/dronekill.cpp "s/    if (c->due) g_any_due.store(true, std::memory_order_release);//" quest-host-combat dronekill_test
runm dk_chase_line_cap src/quest/combat/dronekill.cpp "s/Allow(g_chase_lines, 8, \"chase\")/Allow(g_chase_lines, 9, \"chase\")/" quest-host-combat dronekill_test
runm dk_chase_line_all src/quest/combat/dronekill.cpp "s/  if (moving \&\& Allow(g_chase_lines, 8, \"chase\"))/  if (Allow(g_chase_lines, 8, \"chase\"))/" quest-host-combat dronekill_test
runm dk_local_fns src/quest/combat/dronekill.cpp "s/  if (g_fns.local == nullptr || space == nullptr || g_fns.find == nullptr) return false;/  (void)0;/" quest-host-combat dronekill_test
runm dk_local_space src/quest/combat/dronekill.cpp "s/  if (g_fns.local == nullptr || space == nullptr || g_fns.find == nullptr) return false;/  if (g_fns.local == nullptr || g_fns.find == nullptr) return false;/" quest-host-combat dronekill_test
runm dk_local_handle src/quest/combat/dronekill.cpp "s/!SafeRead(idx + (phys_h \& 0xffff) \* 4ull, \&slot, sizeof slot)) return false;/!SafeRead(idx + phys_h * 4ull, \&slot, sizeof slot)) return false;/" quest-host-combat dronekill_test
runm dk_local_owners src/quest/combat/dronekill.cpp "s/  if (!SafeRead(physics + kPhOwners, \&owners, sizeof owners) || !owners || !SafeRead(owners + slot \* 16ull, \&actor, sizeof actor) || !actor) return false;/  if (!SafeRead(physics + kPhOwners, \&owners, sizeof owners) || !SafeRead(owners + slot * 16ull, \&actor, sizeof actor)) return false;/" quest-host-combat dronekill_test
runm dk_local_stride src/quest/combat/dronekill.cpp "s/owners + slot \* 16ull/owners + slot * 8ull/" quest-host-combat dronekill_test
runm dk_local_np src/quest/combat/dronekill.cpp "s/  if (np == nullptr || !SafeRead(reinterpret_cast<std::uintptr_t>(np) + kNetPhysicsPhys, \&np_phys, sizeof np_phys) || np_phys != physics) return false;/  if (np == nullptr) return false;/;$NOWARN" quest-host-combat dronekill_test
runm dk_local_np_phys src/quest/combat/dronekill.cpp "s/ || np_phys != physics) return false;/) return false;/" quest-host-combat dronekill_test
runm dk_local_none src/quest/combat/dronekill.cpp "s/  if ((h \& 0xffff) == 0xffff) return true;/  if ((h \& 0xffff) == 0xffff) return false;/" quest-host-combat dronekill_test
runm dk_local_none_chk src/quest/combat/dronekill.cpp "s/  if ((h \& 0xffff) == 0xffff) return true;//" quest-host-combat dronekill_test
runm dk_local_result src/quest/combat/dronekill.cpp "s/  return g_fns.local(np, h) != 0;/  return true;/" quest-host-combat dronekill_test
runm dk_local_arg src/quest/combat/dronekill.cpp "s/  return g_fns.local(np, h) != 0;/  return g_fns.local(np, phys_h) != 0;/" quest-host-combat dronekill_test
runm dk_local_sym src/quest/combat/dronekill.cpp "s/CallComponent(component, np, actor, kNetPhysics)/CallComponent(component, np, actor, kDroneActor)/" quest-host-combat dronekill_test
runm dk_tick_inst src/quest/combat/dronekill.cpp "/^void Tick(std::uintptr_t game) noexcept {/,/^}/s/  if (!g_installed.load(std::memory_order_acquire)) return;/  (void)0;/" quest-host-combat dronekill_test
runm dk_tick_first src/quest/combat/dronekill.cpp "/^void Tick(std::uintptr_t game) noexcept {/,/^}/s/  if (n == 0) {/  if (n == 1) {/" quest-host-combat dronekill_test
runm dk_tick_any src/quest/combat/dronekill.cpp "s/  if (!g_any_due.load(std::memory_order_acquire) || g_t_in_tick.Get() != 0) return;/  if (g_t_in_tick.Get() != 0) return;/" quest-host-combat dronekill_test
runm dk_tick_reent src/quest/combat/dronekill.cpp "s/  if (!g_any_due.load(std::memory_order_acquire) || g_t_in_tick.Get() != 0) return;/  if (!g_any_due.load(std::memory_order_acquire)) return;/" quest-host-combat dronekill_test
runm dk_tick_reset src/quest/combat/dronekill.cpp "/^__attribute__((noinline)) void TickBody/,/^}/s/^  g_t_in_tick.Set(0);\$/  (void)0;/" quest-host-combat dronekill_test
runm dk_tick_runs src/quest/combat/dronekill.cpp "s/  g_due_runs.fetch_add(1, std::memory_order_relaxed);/  g_due_runs.fetch_add(0, std::memory_order_relaxed);/" quest-host-combat dronekill_test
runm dk_tick_done src/quest/combat/dronekill.cpp "s/    g_table.Done(due\[i\].number, due\[i\].idx);//" quest-host-combat dronekill_test
runm dk_tick_wait src/quest/combat/dronekill.cpp "s/    if (r == 2) {\$/    if (false) {/" quest-host-combat dronekill_test
runm dk_tick_clear_t src/quest/combat/dronekill.cpp "s/  if (!g_table.AnyDue() \&\& !g_touch.AnyDue() \&\& !g_chases.AnyDue())/  if (!g_table.AnyDue() \&\& !g_chases.AnyDue())/" quest-host-combat dronekill_test
runm dk_tick_clear_g src/quest/combat/dronekill.cpp "s/  if (!g_table.AnyDue() \&\& !g_touch.AnyDue() \&\& !g_chases.AnyDue())/  if (!g_touch.AnyDue() \&\& !g_chases.AnyDue())/" quest-host-combat dronekill_test
runm dk_tick_chase_no src/quest/combat/dronekill.cpp "s/  if (g_chase) ChaseTick(now);//;$NOWARN" quest-host-combat dronekill_test
runm dk_tick_warn src/quest/combat/dronekill.cpp "s/    sentinel::LogFields(LogLevel::kWarn, \"combat_dronekill_tick\",/    sentinel::LogFields(LogLevel::kInfo, \"combat_dronekill_tick\",/" quest-host-combat dronekill_test
runm dk_tick_warn_cond src/quest/combat/dronekill.cpp "s/  if ((expired != 0 || (waiting != 0 \&\& off == 0 \&\& skipped == 0)) \&\& Allow/  if ((waiting != 0 \&\& off == 0 \&\& skipped == 0) \&\& Allow/" quest-host-combat dronekill_test
runm dk_tick_warn_wait src/quest/combat/dronekill.cpp "s/  if ((expired != 0 || (waiting != 0 \&\& off == 0 \&\& skipped == 0)) \&\& Allow/  if ((expired != 0) \&\& Allow/" quest-host-combat dronekill_test
runm dk_ct_due src/quest/combat/dronekill.cpp "s/    if (!c.due) continue;/    (void)0;/" quest-host-combat dronekill_test
runm dk_ct_once src/quest/combat/dronekill.cpp "s/    c.due = false;  \/\/ one write per AttackTarget/    (void)0;  \/\/ one write per AttackTarget/" quest-host-combat dronekill_test
runm dk_ct_fresh src/quest/combat/dronekill.cpp "s/    if (now - c.ms < kChaseMs) due\[n++\] = c;/    if (true) due[n++] = c;/;$NOWARN" quest-host-combat dronekill_test
runm dk_ct_stale src/quest/combat/dronekill.cpp "s/    else c.moving = false;/    else (void)0;/" quest-host-combat dronekill_test
runm dk_ct_ok src/quest/combat/dronekill.cpp "s/    if (!ChaseStillOk(due\[i\])) continue;  \/\/ DF-1/    (void)0;/;$NOWARN" quest-host-combat dronekill_test
runm dk_ct_owner src/quest/combat/dronekill.cpp "s/ || owner != due\[i\].owner) continue;/) continue;/" quest-host-combat dronekill_test
runm dk_ct_flag src/quest/combat/dronekill.cpp "s/ || flag == 0) continue;  \/\/ outside the window/) continue;  \/\/ outside the window/" quest-host-combat dronekill_test
runm dk_ct_flag_rd src/quest/combat/dronekill.cpp "s/    if (!SafeRead(owner + kOwnerFlag, \&flag, sizeof flag) || flag == 0) continue;/    if (flag == 0) continue;/" quest-host-combat dronekill_test
runm dk_ct_arg_v src/quest/combat/dronekill.cpp "s/DkC3{due\[i\].v\[0\], due\[i\].v\[1\], due\[i\].v\[2\]}/DkC3{due[i].v[0], due[i].v[1], 0}/" quest-host-combat dronekill_test
runm dk_ct_arg_phys src/quest/combat/dronekill.cpp "s/g_fns.set_vel(reinterpret_cast<void\*>(due\[i\].physics), due\[i\].phys,/g_fns.set_vel(reinterpret_cast<void*>(due[i].physics), 0,/" quest-host-combat dronekill_test
runm dk_cs_atowner src/quest/combat/dronekill.cpp "s/ || at_owner != c.atOwner) return false;/) return false;/" quest-host-combat dronekill_test
runm dk_cs_live src/quest/combat/dronekill.cpp "s/  if (!HandleLive(c.cs, kAtIdx, c.handle, \&slot)) return false;/  if (false) return false;/" quest-host-combat dronekill_test
runm dk_cs_phys_h src/quest/combat/dronekill.cpp "s/ || phys_h != c.phys) return false;/) return false;/" quest-host-combat dronekill_test
runm dk_cs_physics src/quest/combat/dronekill.cpp "s/ || physics != c.physics) return false;/) return false;/" quest-host-combat dronekill_test
runm dk_cs_plive src/quest/combat/dronekill.cpp "s/  return HandleLive(c.physics, kPhIdx, c.phys, \&pslot);/  return true;/;$NOWARN" quest-host-combat dronekill_test
runm dk_tick_shared_word src/quest/combat/dronekill.cpp "s/^ThreadWord g_t_in_tick;.*/$SHARED_WORD g_t_in_tick;/" quest-host-combat dronekill_test
