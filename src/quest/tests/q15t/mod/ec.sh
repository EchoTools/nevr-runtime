# the `ec` module: pure rules, install / wrappers / step / hide / selection against a fake libr15 image, the page as a shared-object
# export (tests/ec_page_test.cpp), and the NDK static-init check when the NDK is present.
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -rdynamic -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/ec_test.cpp" "$here/src/ec.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/ec_test" -ldl -lpthread
"$out/ec_test"

# the page as a symbol of a RTLD_LOCAL shared object, built with the plugin's flags (-fvisibility=hidden)
mkdir -p "$out/ec_page"
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -fPIC -shared -fvisibility=hidden -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/ec_page_shim.cpp" "$here/src/ec.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/ec_page/libec_page_test.so" -ldl -lpthread
nm -D --defined-only "$out/ec_page/libec_page_test.so" | grep -q ' re33_ec_page$' || { echo "FAIL: re33_ec_page is not in the dynamic symbols of an -fvisibility=hidden build"; false; }
g++ -std=c++17 -g -O1 -Wall -Wextra -Werror "$tests/ec_page_test.cpp" -o "$out/ec_page_test" -ldl
"$out/ec_page_test" "$out/ec_page/libec_page_test.so"
g++ -std=c++17 -g -O1 -fPIC -shared -Wall -Wextra -Werror "$tests/ec_page_other.cpp" -o "$out/ec_page/libec_page_other.so"
"$out/ec_page_test" "$out/ec_page/libec_page_test.so" "$out/ec_page/libec_page_other.so"
# the sentinel exports the page (src/quest/sentinel/ec_page.cpp): a script-lib stand-in that depends on it finds the page the plugin writes
mkdir -p "$out/ec_page/sentinel"
g++ -std=c++17 -g -O1 -fPIC -shared -Wall -Wextra -Werror -Wl,-soname,libec_sentinel.so "$tests/ec_page_other.cpp" -o "$out/ec_page/sentinel/libec_sentinel.so"
g++ -std=c++17 -g -O1 -fPIC -shared -Wall -Wextra -Werror "$tests/ec_page_reader.cpp" -o "$out/ec_page/sentinel/libec_reader.so" \
  -L"$out/ec_page/sentinel" -l:libec_sentinel.so -Wl,-rpath,"$out/ec_page/sentinel" -ldl
"$out/ec_page_test" "$out/ec_page/libec_page_test.so" sentinel "$out/ec_page/sentinel/libec_sentinel.so" "$out/ec_page/sentinel/libec_reader.so"

# the build with default words (CMake's default): a fresh install with no switch file is on
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -rdynamic -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" '-DQ15T_EC_DEFAULT_WORDS="types=VPTO equip hide"' \
  "$tests/ec_test.cpp" "$here/src/ec.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/ec_test_defaults" -ldl -lpthread
"$out/ec_test_defaults"
