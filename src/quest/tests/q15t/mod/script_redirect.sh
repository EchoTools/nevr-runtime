g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/script_redirect_test.cpp" "$here/src/script_redirect.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/script_redirect_test"
"$out/script_redirect_test"
