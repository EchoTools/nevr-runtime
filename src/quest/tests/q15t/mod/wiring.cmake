# plugin.cpp's wiring of the modules: (1) every option on against a fake image (names, levels, return value, tick count);
# (2) the capability matrix, one plugin.cpp per option. Module objects are compiled once, with the plugin's own flags.
if(NOT Q15T_ARM64)
  set(_wmods plugin_log script_redirect import_hook combat_map game_tick safe_read got_swap weapon_arm gamespace fxtrail voipdrop custctx droneguard peerclamp gunconnect aibots bvh wallfire impactsnd dronekill dronemuzzle hudmeter shieldgamma levelname boosterpool music sensorwd decals)
  set(_wsources "")
  foreach(m IN LISTS _wmods)
    list(APPEND _wsources "${Q15T_PLUGIN_SRC}/${m}.cpp")
  endforeach()
  set(_wopts -g -O0 -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
  add_library(nevr_q15t_wiring_modules OBJECT ${_wsources})
  target_compile_options(nevr_q15t_wiring_modules PRIVATE ${_wopts})
  target_include_directories(nevr_q15t_wiring_modules PRIVATE "${Q15T_PLUGIN_SRC}" "${Q15T_SDK_INCLUDE}")
  add_dependencies(mod_wiring nevr_q15t_wiring_modules)
endif()

set(_wall Q15T_FXTRAIL=1 Q15T_VOIPDROP=1 Q15T_CUSTCTX=1 Q15T_DRONEGUARD=1 Q15T_PEERCLAMP=1 Q15T_GUNCONNECT=1 Q15T_AIBOTS=1 Q15T_BVH=1 Q15T_WALLFIRE=1 Q15T_IMPACTSND=1 Q15T_DRONEKILL=1 Q15T_DRONEMUZZLE=1 Q15T_HUDMETER=1 Q15T_SHIELDGAMMA=2 Q15T_LEVELNAME=1 Q15T_MUSIC=1 Q15T_SENSORWD=1)
set(_wslots)
set(_wbuild "Q15T_BUILD_ID_FOR_TEST=\"${Q15T_BUILD_ID_FOR_TEST}\"")
set(_winclude "SHELL:-include ${Q15T_TESTS_DIR}/host_overrides.h")

# wiring_program(<program> <defs>...): wiring_test.cpp and plugin.cpp built with the defines, linked with the module objects
function(wiring_program program)
  q15t_host_test(${program} TEST wiring_test.cpp SRC plugin BASE_OPTS ${_wopts} OPTS "${_winclude}"
    OBJECTS $<TARGET_OBJECTS:nevr_q15t_wiring_modules> DEFS ${ARGN} LINK_OPTS -rdynamic LIBS dl pthread)
endfunction()

wiring_program(wiring_test ${_wall} Q15T_T26=0 ${_wslots} ${_wbuild})
# (1b) only bvh on: its tick is the only registration, and it alone must make InitEx install the game tick
wiring_program(wiring_bvh_only WIRING_BVH_ONLY Q15T_BVH=1 ${_wslots} ${_wbuild})
# capability matrix: <label> <expect ALTERS_GAMEPLAY> <expect NETWORK> <defines>...; nothing else on
function(wiring_caps label alters network)
  string(MAKE_C_IDENTIFIER "${label}" id)
  wiring_program(wiring_caps_${id} WIRING_CAPS_ONLY "WIRING_LABEL=\"${label}\"" EXPECT_ALTERS=${alters} EXPECT_NETWORK=${network} ${ARGN} ${_wslots})
endfunction()
wiring_caps(none 0 0 Q15T_T26=0)
foreach(c FXTRAIL VOIPDROP CUSTCTX IMPACTSND HUDMETER MUSIC SENSORWD DECALS)
  wiring_caps(${c} 0 0 Q15T_${c}=1)
endforeach()
wiring_caps(SHIELDGAMMA=1 0 0 Q15T_SHIELDGAMMA=1)
wiring_caps(SHIELDGAMMA=2 0 0 Q15T_SHIELDGAMMA=2)
foreach(c DRONEGUARD PEERCLAMP DRONEKILL DRONEMUZZLE GUNCONNECT AIBOTS BVH WALLFIRE LEVELNAME T26)
  wiring_caps(${c} 1 0 Q15T_${c}=1)
endforeach()
