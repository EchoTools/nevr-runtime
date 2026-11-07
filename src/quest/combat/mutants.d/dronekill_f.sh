# dronekill (part 6 of 6): each rule of src/quest/combat/dronekill.cpp / dronekill_core.h removed once; src/quest/tests/dronekill_test.cpp must fail on every one
runm dk_hl_gen src/quest/combat/dronekill.cpp "s/  if (e\[1\] != static_cast<std::uint16_t>(handle >> 16)) return false;/  (void)0;/" quest-host-combat dronekill_test
runm dk_hl_mask src/quest/combat/dronekill.cpp "s/!SafeRead(idx + (handle \& 0xffff) \* 4ull, e, sizeof e)) return false;/!SafeRead(idx + handle * 4ull, e, sizeof e)) return false;/" quest-host-combat dronekill_test
runm dk_hl_slot src/quest/combat/dronekill.cpp "s/  \*slot_out = e\[0\];/  *slot_out = e[1];/" quest-host-combat dronekill_test
runm dk_k_inst src/quest/combat/dronekill.cpp "/^void OnKilled/,/^}/s/  if (!g_installed.load(std::memory_order_acquire) || !g_inScope.load(std::memory_order_relaxed)) return;/  if (!g_inScope.load(std::memory_order_relaxed)) return;/" quest-host-combat dronekill_test
runm dk_k_kill src/quest/combat/dronekill.cpp "/^void OnKilled/,/^}/s/  if (g_kill) {/  if (true) {/" quest-host-combat dronekill_test
runm dk_k_nokill src/quest/combat/dronekill.cpp "/^void OnKilled/,/^}/s/  if (g_kill) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_k_due src/quest/combat/dronekill.cpp "s/      if (g->due) g_any_due.store(true, std::memory_order_release);//" quest-host-combat dronekill_test
runm dk_k_gun src/quest/combat/dronekill.cpp "s/      if (g_gun) g_guns.Set(g->actor, 0);  \/\/ M4/      (void)0;  \/\/ M4/" quest-host-combat dronekill_test
runm dk_k_dueflag src/quest/combat/dronekill.cpp "s/g_table.Killed(index, NowMs(), true)/g_table.Killed(index, NowMs(), false)/" quest-host-combat dronekill_test
runm dk_k_now src/quest/combat/dronekill.cpp "s/g_table.Killed(index, NowMs(), true)/g_table.Killed(index, 0, true)/" quest-host-combat dronekill_test
runm dk_k_else src/quest/combat/dronekill.cpp "s/  } else if (Group\* g = g_table.Killed(index)) {/  } else if (Group* g = nullptr) {/" quest-host-combat dronekill_test
runm dk_k_live src/quest/combat/dronekill.cpp "s/  live_n = g_table.Live(live, kGroups);/  live_n = 0; (void)live;/" quest-host-combat dronekill_test
runm dk_k_result src/quest/combat/dronekill.cpp "s/{\"result\", group == 0 ? \"no_live_group\" :/{\"result\", group == 0 ? \"shut_down\" :/" quest-host-combat dronekill_test
runm dk_k_idx src/quest/combat/dronekill.cpp "s/{{\"index\", index \& 0xffff}, {\"tid\", tid}, {\"group\", group},/{{\"index\", index}, {\"tid\", tid}, {\"group\", group},/" quest-host-combat dronekill_test
