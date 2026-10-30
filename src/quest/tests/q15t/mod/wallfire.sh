g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$tests" -I"$nevr/sdk/include" \
  "$tests/wallfire_test.cpp" "$here/src/wallfire.cpp" "$here/src/bvh.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/wallfire_test" -ldl -lpthread
timeout 60 "$out/wallfire_test"
