# the game-tick table: 16 callbacks fit and run in order, the 17th is refused
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/game_tick_test.cpp" "$here/src/game_tick.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" \
  "$here/src/plugin_log.cpp" -o "$out/game_tick_test" -ldl
"$out/game_tick_test"
