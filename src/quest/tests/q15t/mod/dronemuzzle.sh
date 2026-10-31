# host test of the drone muzzle module (plain functions, install, frame tick; the arm64 thunks are stood in for on the host), then the
# arm64 cross-compile of the thunks and their comparison with the source build's
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/dronemuzzle_test.cpp" "$here/src/dronemuzzle.cpp" "$here/src/dronekill.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" \
  "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" -o "$out/dronemuzzle_test" -ldl -lpthread
"$out/dronemuzzle_test"

ndk_bin=""
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
  for d in "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin; do
    if [ -x "$d/aarch64-linux-android26-clang++" ]; then ndk_bin="$d"; fi
  done
fi
if [ "${Q15T_SKIP_NDK:-0}" = 1 ]; then
  echo "dronemuzzle: Q15T_SKIP_NDK=1: the arm64 compile and the thunk comparison were NOT run"
else
  [ -n "${ANDROID_NDK_HOME:-}" ] || { echo "dronemuzzle: ANDROID_NDK_HOME is not set (point it at an Android NDK r26d install, or set Q15T_SKIP_NDK=1 to skip the arm64 check)"; exit 1; }
  [ -n "$ndk_bin" ] || { echo "dronemuzzle: NDK r26d clang not found (set ANDROID_NDK_HOME, or Q15T_SKIP_NDK=1 to skip the arm64 check)"; exit 1; }
  cxx="$ndk_bin/aarch64-linux-android26-clang++"
  mkdir -p "$out/ndk"
  "$cxx" -std=c++17 -fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra -Werror -O2 -I"$here/src" -I"$nevr/sdk/include" \
    -c "$here/src/dronemuzzle.cpp" -o "$out/ndk/dronemuzzle.o"
  if "$ndk_bin/llvm-readelf" -S "$out/ndk/dronemuzzle.o" | grep -q "init_array"; then echo "dronemuzzle: arm64 object has .init_array (a static constructor)"; exit 1; fi
  echo "dronemuzzle: arm64 compile ok, no .init_array"
  # the two naked thunks are in the object: instruction words and relocations, at least 38 lines
  thunk_lines=$("$ndk_bin/llvm-objdump" -dr "$out/ndk/dronemuzzle.o" | awk '/<q15t_dm_(tp|fire)_thunk>:/{p=1} p&&/^$/{p=0} p' \
    | sed -E 's/^ *[0-9a-f]+:\s+//; s/<[^>]*>//g; s/0x[0-9a-f]+//g; s/[[:space:]]+/ /g; s/^ ?[0-9a-f]{16}:? ?//' | grep -v '^$' | wc -l)
  [ "$thunk_lines" -ge 38 ] || { echo "dronemuzzle: the thunks were not found in the arm64 object"; exit 1; }
  echo "dronemuzzle: arm64 thunks present ($thunk_lines lines of instruction words and relocations)"
fi
