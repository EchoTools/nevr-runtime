g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -pthread -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/sensorwd_test.cpp" "$here/src/sensorwd.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/sensorwd_test" -ldl
timeout 30 "$out/sensorwd_test"
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
. "$tests/mod/pbhud_ndk.sh"
pbhud_ndk_check sensorwd
