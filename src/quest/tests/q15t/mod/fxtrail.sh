g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/fxtrail_test.cpp" "$here/src/fxtrail.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/fxtrail_test" -ldl
timeout 60 "$out/fxtrail_test"
