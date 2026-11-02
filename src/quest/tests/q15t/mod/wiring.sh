# plugin.cpp's wiring of the modules: (1) every option on against a fake image (names, levels, return value, tick count);
# (2) the capability matrix, one plugin.cpp per option. Module objects are compiled once, with the plugin's own flags.
_w="$out/wiring"
mkdir -p "$_w/obj"
_wf="-std=c++17 -g -O0 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I$here/src -I$nevr/sdk/include"
_wmods="plugin_log script_redirect import_hook clock_levels plugin_args refresh_rate game_tick safe_read headset_gates got_swap weapon_arm gamespace fxtrail voipdrop custctx droneguard peerclamp gunconnect aibots bvh wallfire impactsnd ec ecfx dronekill dronemuzzle powerboost hudmeter shieldgamma levelname boosterpool music sensorwd decals"
_wobjs=""
for _m in $_wmods; do
  g++ $_wf -c "$here/src/$_m.cpp" -o "$_w/obj/$_m.o"
  _wobjs="$_wobjs $_w/obj/$_m.o"
done
_wall="-DQ15T_FXTRAIL=1 -DQ15T_VOIPDROP=1 -DQ15T_CUSTCTX=1 -DQ15T_DRONEGUARD=1 -DQ15T_PEERCLAMP=1 -DQ15T_GUNCONNECT=1 -DQ15T_AIBOTS=1 -DQ15T_BVH=1 -DQ15T_WALLFIRE=1 -DQ15T_IMPACTSND=1 -DQ15T_EC=1 -DQ15T_ECFX=1 -DQ15T_DRONEKILL=1 -DQ15T_DRONEMUZZLE=1 -DQ15T_POWERBOOST=3 -DQ15T_HUDMETER=1 -DQ15T_SHIELDGAMMA=2 -DQ15T_LEVELNAME=1 -DQ15T_MUSIC=1 -DQ15T_SENSORWD=1"
_wslots="-DQ15T_SLOT_SETCLOCKLEVELS=0x10 -DQ15T_SLOT_SETDISPLAYREFRESHRATE=0x18 -include $tests/host_overrides.h"
g++ $_wf -rdynamic $_wall -DQ15T_HZ_MATCH90=1 -DQ15T_T26=0 $_wslots -DQ15T_BUILD_ID_FOR_TEST='"b243509c08ce677aeb95fa348016949b3fc45230"' \
  "$tests/wiring_test.cpp" "$here/src/plugin.cpp" $_wobjs -o "$_w/wiring_test" -ldl -lpthread
"$_w/wiring_test"

# (1b) only bvh on: its tick is the only registration, and it alone must make InitEx install the game tick
g++ $_wf -rdynamic -DWIRING_BVH_ONLY -DQ15T_BVH=1 $_wslots -DQ15T_BUILD_ID_FOR_TEST='"b243509c08ce677aeb95fa348016949b3fc45230"' \
  "$tests/wiring_test.cpp" "$here/src/plugin.cpp" $_wobjs -o "$_w/wiring_bvh_only" -ldl -lpthread
"$_w/wiring_bvh_only"

# (1c) the headset caller-range gates alone: the link gate, and the link gate with the bots gate
g++ $_wf -rdynamic -DWIRING_GATE_LINK_ONLY -DQ15T_GATE_LINK=1 $_wslots -DQ15T_BUILD_ID_FOR_TEST='"b243509c08ce677aeb95fa348016949b3fc45230"' \
  "$tests/wiring_test.cpp" "$here/src/plugin.cpp" $_wobjs -o "$_w/wiring_gate_link" -ldl -lpthread
"$_w/wiring_gate_link"
# the bots gate is installed only with levelname (combat_gates.h): built with it, and then with levelname.off in the data directory
g++ $_wf -rdynamic -DWIRING_GATE_LINK_ONLY -DEXPECT_BOTS -DQ15T_GATE_LINK=1 -DQ15T_GATE_BOTS=1 -DQ15T_LEVELNAME=1 $_wslots -DQ15T_BUILD_ID_FOR_TEST='"b243509c08ce677aeb95fa348016949b3fc45230"' \
  "$tests/wiring_test.cpp" "$here/src/plugin.cpp" $_wobjs -o "$_w/wiring_gate_link_bots" -ldl -lpthread
"$_w/wiring_gate_link_bots"
g++ $_wf -rdynamic -DWIRING_GATE_LINK_ONLY -DEXPECT_BOTS -DWIRING_LEVELNAME_OFF -DQ15T_GATE_LINK=1 -DQ15T_GATE_BOTS=1 -DQ15T_LEVELNAME=1 $_wslots -DQ15T_BUILD_ID_FOR_TEST='"b243509c08ce677aeb95fa348016949b3fc45230"' \
  "$tests/wiring_test.cpp" "$here/src/plugin.cpp" $_wobjs -o "$_w/wiring_gate_bots_levelname_off" -ldl -lpthread
"$_w/wiring_gate_bots_levelname_off"

# capability matrix: <label> <defines> <expect ALTERS_GAMEPLAY> <expect NETWORK>; nothing else on
_wcaps() {
  g++ $_wf -rdynamic -DWIRING_CAPS_ONLY -DWIRING_LABEL="\"$1\"" -DEXPECT_ALTERS=$3 -DEXPECT_NETWORK=$4 $2 $_wslots \
    "$tests/wiring_test.cpp" "$here/src/plugin.cpp" $_wobjs -o "$_w/caps_test" -ldl -lpthread
  "$_w/caps_test"
}
_wcaps none "-DQ15T_T26=0" 0 0
for _c in FXTRAIL VOIPDROP CUSTCTX IMPACTSND ECFX HUDMETER MUSIC SENSORWD DECALS; do _wcaps "$_c" "-DQ15T_${_c}=1" 0 0; done
_wcaps SHIELDGAMMA=1 "-DQ15T_SHIELDGAMMA=1" 0 0
_wcaps SHIELDGAMMA=2 "-DQ15T_SHIELDGAMMA=2" 0 0
for _c in DRONEGUARD PEERCLAMP DRONEKILL DRONEMUZZLE GUNCONNECT AIBOTS BVH WALLFIRE LEVELNAME T26 HZ_MATCH90 GATE_LINK GATE_PERM GATE_BOTS; do
  _wcaps "$_c" "-DQ15T_${_c}=1" 1 0
done
for _c in 1 2 3; do _wcaps "POWERBOOST=$_c" "-DQ15T_POWERBOOST=$_c" 1 0; done
_wcaps EC "-DQ15T_EC=1" 1 1
