# the `decals` module: the table parser, the per-system state machine and the wrappers over a stand-in engine (a fake bullet system,
# allocator, node, mesh and slot page), then the arm64 build with the NDK clang (no static initializer)
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/decals_test.cpp" "$here/src/decals.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/decals_test" -ldl -lpthread
timeout 60 "$out/decals_test"
. "$tests/mod/pbhud_ndk.sh"
pbhud_ndk_check decals
