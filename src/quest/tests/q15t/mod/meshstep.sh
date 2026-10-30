g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/meshstep_test.cpp" "$here/src/meshstep.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  "$here/src/import_hook.cpp" \
  -o "$out/meshstep_test" -ldl
"$out/meshstep_test"
