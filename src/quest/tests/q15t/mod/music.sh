g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/music_test.cpp" "$here/src/music.cpp" "$here/src/refresh_rate.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/music_test" -ldl
timeout 60 "$out/music_test"
# the arm64 build: compiles with the NDK clang, no static initializer (.init_array / .ctors)
. "$tests/mod/pbhud_ndk.sh"
pbhud_ndk_check music
