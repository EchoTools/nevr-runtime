# The early plugin: the real entry points (src/plugin_early.cpp) against the heaps test's fake game library; then the arm64 build with
# the repack's static-init check (the plugin file must have no constructors).
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/early_test.cpp" "$here/src/plugin_early.cpp" "$here/src/heaps.cpp" "$here/src/memsys.cpp" "$here/src/import_hook.cpp" \
  "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" -o "$out/early_test" -ldl
"$out/early_test"
. "$tests/mod/pbhud_ndk.sh"
pbhud_ndk_check plugin_early
