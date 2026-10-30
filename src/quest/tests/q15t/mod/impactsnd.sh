g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -pthread -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/impactsnd_test.cpp" "$here/src/impactsnd.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/impactsnd_test" -ldl
"$out/impactsnd_test"
