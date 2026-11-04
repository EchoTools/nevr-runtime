# the game-tick table: 16 callbacks fit and run in order, the 17th is refused
q15t_host_test(game_tick_test TEST game_tick_test.cpp SRC game_tick import_hook got_swap safe_read plugin_log LIBS dl)
