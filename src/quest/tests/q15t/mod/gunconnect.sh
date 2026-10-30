g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/gunconnect_test.cpp" "$here/src/gunconnect.cpp" "$here/src/gamespace.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/gunconnect_test" -ldl
"$out/gunconnect_test"
