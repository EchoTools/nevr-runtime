# dronekill (part 6 of 7): each rule of src/quest/combat/dronekill.cpp / dronekill_core.h removed once; src/quest/tests/dronekill_test.cpp must fail on every one
runm dk_hl_gen src/quest/combat/dronekill.cpp "s/  if (e\[1\] != static_cast<std::uint16_t>(handle >> 16)) return false;/  (void)0;/" quest-host-combat dronekill_test
runm dk_hl_mask src/quest/combat/dronekill.cpp "s/!SafeRead(idx + (handle \& 0xffff) \* 4ull, e, sizeof e)) return false;/!SafeRead(idx + handle * 4ull, e, sizeof e)) return false;/" quest-host-combat dronekill_test
runm dk_hl_slot src/quest/combat/dronekill.cpp "s/  \*slot_out = e\[0\];/  *slot_out = e[1];/" quest-host-combat dronekill_test
runm dk_k_inst src/quest/combat/dronekill.cpp "/^void OnKilled/,/^}/s/  if (!g_installed.load(std::memory_order_acquire) || !g_inScope.load(std::memory_order_relaxed)) return;/  if (!g_inScope.load(std::memory_order_relaxed)) return;/" quest-host-combat dronekill_test
runm dk_k_kill src/quest/combat/dronekill.cpp "/^void OnKilled/,/^}/s/  if (g_kill) {/  if (true) {/" quest-host-combat dronekill_test
runm dk_k_nokill src/quest/combat/dronekill.cpp "/^void OnKilled/,/^}/s/  if (g_kill) {/  if (false) {/" quest-host-combat dronekill_test
runm dk_k_due src/quest/combat/dronekill.cpp "s/      if (g->x.due) g_any_due.store(true, std::memory_order_release);//" quest-host-combat dronekill_test
runm dk_k_gun src/quest/combat/dronekill.cpp "s/      if (g_gun) HideGunOnKill(g_guns);  \/\/ M4/      (void)0;  \/\/ M4/" quest-host-combat dronekill_test
runm dk_k_dueflag src/quest/combat/dronekill.cpp "s/g_table.Killed(index, NowMs(), true)/g_table.Killed(index, NowMs(), false)/" quest-host-combat dronekill_test
runm dk_k_now src/quest/combat/dronekill.cpp "s/g_table.Killed(index, NowMs(), true)/g_table.Killed(index, 0, true)/" quest-host-combat dronekill_test
runm dk_k_else src/quest/combat/dronekill.cpp "s/  } else if (Group\* g = g_table.Killed(index)) {/  } else if (Group* g = nullptr) {/" quest-host-combat dronekill_test
runm dk_k_live src/quest/combat/dronekill.cpp "s/  live_n = g_table.Live(live, kGroups);/  live_n = 0; (void)live;/" quest-host-combat dronekill_test
runm dk_k_result src/quest/combat/dronekill.cpp "s/{\"result\", group == 0 ? \"no_live_group\" :/{\"result\", group == 0 ? \"shut_down\" :/" quest-host-combat dronekill_test
runm dk_k_idx src/quest/combat/dronekill.cpp "s/{{\"index\", index \& 0xffff}, {\"tid\", tid}, {\"group\", group},/{{\"index\", index}, {\"tid\", tid}, {\"group\", group},/" quest-host-combat dronekill_test
# D11 and the stats record (#2359): the drone's end is the table's dead-marking call; the expired lines are capped at 16 with one notice; each counter
# of combat_dronekill_stats must reach the record; the record is written every kStatsEvery drone Enters and once at the process end.
runm dk_d11_off src/quest/combat/dronekill.cpp "s/    const int dead = g_table.Off(reinterpret_cast<std::uint64_t>(system), handle);/    const int dead = 0;/" quest-host-combat dronekill_test
runm dk_d11_alive src/quest/combat/dronekill_core.h "s/        return m.group < 0 || !m.wasAlive ? 0 : t.g\[m.group\].x.number;/        return m.group < 0 ? 0 : t.g[m.group].x.number;/" quest-host-combat dronekill_test
runm dk_expired_cap17 src/quest/combat/dronekill.cpp "s/constexpr unsigned kExpiredLines = 16;/constexpr unsigned kExpiredLines = 17;/" quest-host-combat dronekill_test
runm dk_expired_count src/quest/combat/dronekill.cpp "s/      g_expired.fetch_add(1, std::memory_order_relaxed);//" quest-host-combat dronekill_test
runm dk_stats_enter_drone src/quest/combat/dronekill.cpp "s/{\"enter_drone\", s.enter_drone}/{\"enter_drone\", 0u}/" quest-host-combat dronekill_test
runm dk_stats_enter_other src/quest/combat/dronekill.cpp "s/{\"enter_other\", s.enter_other}/{\"enter_other\", 0u}/" quest-host-combat dronekill_test
runm dk_stats_fires src/quest/combat/dronekill.cpp "s/{\"fires\", s.fires}/{\"fires\", 0u}/" quest-host-combat dronekill_test
runm dk_stats_attacks src/quest/combat/dronekill.cpp "s/{\"attacks\", s.attacks}/{\"attacks\", 0u}/" quest-host-combat dronekill_test
runm dk_stats_ticks src/quest/combat/dronekill.cpp "s/{\"ticks\", s.ticks}/{\"ticks\", 0u}/" quest-host-combat dronekill_test
runm dk_stats_due_runs src/quest/combat/dronekill.cpp "s/{\"due_runs\", s.due_runs}/{\"due_runs\", 0u}/" quest-host-combat dronekill_test
runm dk_stats_expired src/quest/combat/dronekill.cpp "s/{\"expired\", s.expired}/{\"expired\", 0u}/" quest-host-combat dronekill_test
runm dk_stats_periodic src/quest/combat/dronekill.cpp "s/  if ((drones \& (kStatsEvery - 1)) == 0) LogStats(false);/  (void)drones;/" quest-host-combat dronekill_test
runm dk_stats_final src/quest/combat/dronekill.cpp "s/  if (g_installed.load(std::memory_order_acquire)) LogStats(true);/  (void)0;/" quest-host-combat dronekill_test
