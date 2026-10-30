g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$tests" \
  "$tests/bvh_core_test.cpp" -o "$out/bvh_core_test"
timeout 30 "$out/bvh_core_test"
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$tests" -I"$nevr/sdk/include" \
  "$tests/bvh_test.cpp" "$here/src/bvh.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/bvh_test" -ldl -lpthread
timeout 60 "$out/bvh_test"
