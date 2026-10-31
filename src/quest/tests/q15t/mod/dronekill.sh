# host test of the drone kill / lock-on / gun / chase / shot module, then the arm64 cross-compile and the static-init check
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/dronekill_test.cpp" "$here/src/dronekill.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" \
  "$here/src/plugin_log.cpp" -o "$out/dronekill_test" -ldl -lpthread
"$out/dronekill_test"

# arm64: the module compiles with the plugin's own flags and leaves no static constructor (the repack refuses a plugin with one).
# Q15T_SKIP_NDK=1 is the only way to skip; a missing toolchain without it fails.
ndk_bin=""
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
  for d in "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin; do
    if [ -x "$d/aarch64-linux-android26-clang++" ]; then ndk_bin="$d"; fi
  done
fi
if [ "${Q15T_SKIP_NDK:-0}" = 1 ]; then
  echo "dronekill: Q15T_SKIP_NDK=1: the arm64 compile and the static-init check were NOT run"
else
  [ -n "${ANDROID_NDK_HOME:-}" ] || { echo "dronekill: ANDROID_NDK_HOME is not set (point it at an Android NDK r26d install, or set Q15T_SKIP_NDK=1 to skip the arm64 check)"; exit 1; }
  [ -n "$ndk_bin" ] || { echo "dronekill: NDK r26d clang not found (set ANDROID_NDK_HOME, or Q15T_SKIP_NDK=1 to skip the arm64 check)"; exit 1; }
  mkdir -p "$out/ndk"
  "$ndk_bin/aarch64-linux-android26-clang++" -std=c++17 -fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra -Werror -O2 \
    -I"$here/src" -I"$nevr/sdk/include" -c "$here/src/dronekill.cpp" -o "$out/ndk/dronekill.o"
  if "$ndk_bin/llvm-readelf" -S "$out/ndk/dronekill.o" | grep -q "init_array"; then echo "dronekill: arm64 object has .init_array (a static constructor)"; exit 1; fi
  echo "dronekill: arm64 compile ok, no .init_array"
fi
