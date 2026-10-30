g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/aibots_test.cpp" "$here/src/aibots.cpp" "$here/src/refresh_rate.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" \
  "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/aibots_test" -ldl
"$out/aibots_test"
