# plugin.cpp's wiring of the modules, judged by mod/wiring.cmake (tests/wiring_test.cpp); the tick table, judged by mod/game_tick.cmake.
runm wiring wire_init_nonzero      src/plugin.cpp "/never this return value\./{n;s/return 0;/return 1;/}"
runm wiring wire_caps_gameplay     src/plugin.cpp "s/uint32_t caps = NEVR_PLUGIN_CAP_HOOKS_ENGINE;/uint32_t caps = NEVR_PLUGIN_CAP_HOOKS_ENGINE | NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY;/"
runm wiring wire_modules_named     src/plugin.cpp "s/constexpr char kCompiledModules\[\] = \"\";/constexpr char kCompiledModules[] = \"levelname \";/"
# the tick table (tests/game_tick_test.cpp)
runm game_tick tick_max_8          src/game_tick.h   "s/inline constexpr int kMaxGameTicks = 16;/inline constexpr int kMaxGameTicks = 8;/"
runm game_tick tick_max_17         src/game_tick.h   "s/inline constexpr int kMaxGameTicks = 16;/inline constexpr int kMaxGameTicks = 17;/"
runm game_tick tick_cap_off_by_one src/game_tick.cpp "s/if (i >= kMaxTicks) return false;/if (i > kMaxTicks) return false;/"
runm game_tick tick_cap_ignored    src/game_tick.cpp "s/if (i >= kMaxTicks) return false;/if (i >= 1000) return false;/"
runm game_tick tick_reverse_order  src/game_tick.cpp "s/for (int i = 0; i < n; ++i) {/for (int i = n - 1; i >= 0; --i) {/"
runm game_tick tick_last_skipped   src/game_tick.cpp "s/for (int i = 0; i < n; ++i) {/for (int i = 0; i < n - 1; ++i) {/"
runm game_tick tick_before_real    src/game_tick.cpp "s/  if (real != nullptr) real(game);//"
runm game_tick tick_count_wrong    src/game_tick.cpp "s/int GameTickCount() { return g_count.load(); }/int GameTickCount() { return 0; }/"
