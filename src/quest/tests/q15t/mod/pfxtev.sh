g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/pfxtev_test.cpp" "$here/src/pfxtev.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/pfxtev_test" -ldl
timeout 30 "$out/pfxtev_test"
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
. "$tests/mod/pbhud_ndk.sh"
pbhud_ndk_check pfxtev
