# the `ecfx` module: the NEUTRON queue rules and the attach / tick / gate against a stand-in wall lib (soname libac4e9374a146d3a4.so)
# loaded for real, in two runs: a complete lib and one that lacks the burst entry.
mkdir -p "$out/ecfx_full" "$out/ecfx_missing"
g++ -std=c++17 -g -O1 -shared -fPIC -Wl,-soname,libac4e9374a146d3a4.so "$tests/ecfx_wall_stub.cpp" -o "$out/ecfx_full/libac4e9374a146d3a4.so"
g++ -std=c++17 -g -O1 -shared -fPIC -DSTUB_MISSING_BURST -Wl,-soname,libac4e9374a146d3a4.so "$tests/ecfx_wall_stub.cpp" -o "$out/ecfx_missing/libac4e9374a146d3a4.so"
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -rdynamic -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/ecfx_test.cpp" "$here/src/ecfx.cpp" "$here/src/ec.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/ecfx_test" -ldl -lpthread
"$out/ecfx_test" full "$out/ecfx_full/libac4e9374a146d3a4.so"
"$out/ecfx_test" missing "$out/ecfx_missing/libac4e9374a146d3a4.so"

# the default-on build (CMake's default): no neutronfx.on needed
g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -rdynamic -Wall -Wextra -Werror -DQ15T_ECFX_DEFAULT_ON=1 -I"$here/src" -I"$nevr/sdk/include" \
  "$tests/ecfx_test.cpp" "$here/src/ecfx.cpp" "$here/src/ec.cpp" "$here/src/import_hook.cpp" "$here/src/got_swap.cpp" "$here/src/safe_read.cpp" "$here/src/plugin_log.cpp" \
  -o "$out/ecfx_test_defaulton" -ldl -lpthread
"$out/ecfx_test_defaulton" full "$out/ecfx_full/libac4e9374a146d3a4.so"
