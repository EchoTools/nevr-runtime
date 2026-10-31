# Sourced by tests/mod/{powerboost,hudmeter,shieldgamma}.sh with $here, $nevr, $out set.
# pbhud_ndk_check <module>...: cross-compile src/<module>.cpp with the NDK r26d clang for arm64 (-fno-exceptions -fno-rtti,
# the plugin's own warning flags) and fail unless the object has no static initializer section. Fails loudly when the NDK is
# missing (set Q15T_SKIP_NDK=1 to skip it explicitly; the skip is printed).
pbhud_ndk_check() {
  local ndk="${Q15T_NDK:-}"
  if [ -z "$ndk" ]; then
    if [ "${Q15T_SKIP_NDK:-0}" = 1 ]; then echo "SKIPPED (Q15T_SKIP_NDK=1): Q15T_NDK is not set, arm64 build not checked"; return 0; fi
    echo "FAIL: Q15T_NDK is not set (point it at an Android NDK r26d install, or set Q15T_SKIP_NDK=1 to skip explicitly)"; return 1
  fi
  local cxx; cxx="$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android26-clang++ 2>/dev/null | { read -r l; echo "$l"; })"
  if [ -z "$cxx" ]; then
    if [ "${Q15T_SKIP_NDK:-0}" = 1 ]; then echo "SKIPPED (Q15T_SKIP_NDK=1): no NDK at $ndk, arm64 build not checked"; return 0; fi
    echo "FAIL: NDK clang not found under $ndk (set Q15T_NDK, or Q15T_SKIP_NDK=1 to skip explicitly)"; return 1
  fi
  local readelf; readelf="$(dirname "$cxx")/llvm-readelf"
  local m
  for m in "$@"; do
    "$cxx" -std=c++17 -O2 -fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
      -c "$here/src/$m.cpp" -o "$out/$m.arm64.o" || { echo "FAIL: arm64 compile of $m"; return 1; }
    if "$readelf" -S "$out/$m.arm64.o" | grep -q -E '\.init_array|\.ctors'; then echo "FAIL: $m.arm64.o has a static initializer section"; return 1; fi
    echo "arm64 build of $m: ok (NDK r26d clang++, no .init_array/.ctors)"
  done
}
