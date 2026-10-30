# the `pfxshotpos` module: the probe's core (id classes, bins, line budget, jump test, summary cadence) and its install step (the kill
# file, and the missing-renderer WARNING), over a switch directory under the build output; then the arm64 build with the NDK clang
mkdir -p "$out/pfxshotpos_switch"
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/pfxshotpos_test.cpp" "$here/src/pfxshotpos.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/pfxshotpos_test" -ldl
timeout 30 "$out/pfxshotpos_test" "$out/pfxshotpos_switch"
. "$tests/mod/pbhud_ndk.sh"
pbhud_ndk_check pfxshotpos
