g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/t26_test.cpp" "$here/src/weapon_arm.cpp" "$here/src/gamespace.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/t26_test" -ldl
"$out/t26_test"
