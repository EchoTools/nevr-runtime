# The q15t Quest plugin: libq15t.so (the modules).
# Added by src/quest/integration/CMakeLists.txt, which defines nevr::plugin_sdk and nevr_pinned_build_id. The library is in no
# release or package-dev APK; the variant and combat recipes package it (src/quest/plugins/q15t/README.md). Paths are relative to this file.
set(Q15T_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ---- Gameplay and fix modules (src/<m>.{h,cpp}; docs/quest/q15t/modules/<m>.md). Defaults are each doc's own, i.e. the source build's. ----
# ON: the source build installs it unless switched off (a runtime off-file in the plugin data directory is kept where the doc keeps it).
# The runtime owns fxtrail, voipdrop, custctx, droneguard and peerclamp (#2178); hudmeter and shieldgamma (#2186); impactsnd and decals (#2183); ec, ecfx, POWER BOOST, dronekill and dronemuzzle (#2187); gunconnect, aibots and weapon_arm (#2185):
# src/quest/combat/, keys combat.impactsnd, combat.impactsnd_write, combat.decals, combat.decals_flip_u, combat.decals_flip_v, combat.ec,
# combat.ec_words, combat.ecfx, combat.powerboost_stage, combat.dronekill, combat.dronemuzzle, combat.gunconnect, combat.aibots, combat.aibots_scenario, combat.weapon_arm, combat.hudmeter, combat.shieldgamma_gamma,
# combat.shieldgamma_tube_mirror, combat.fxtrail, combat.voipdrop, combat.custctx, combat.droneguard, combat.droneguard_stage1, combat.peerclamp.
option(Q15T_LEVELNAME "levelname (R04, G7) with boosterpool: global_mp keeps its plain name, so the full-size global_mp in the pack loads. Source combat build: on. Kill file levelname.off" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(Q15T_SOURCES src/plugin.cpp src/plugin_log.cpp src/script_redirect.cpp src/import_hook.cpp src/combat_map.cpp src/game_tick.cpp src/safe_read.cpp src/got_swap.cpp src/gamespace.cpp)
foreach(m LEVELNAME)
  if(Q15T_${m})
    string(TOLOWER ${m} lm)
    list(APPEND Q15T_SOURCES src/${lm}.cpp)
  endif()
endforeach()
if(Q15T_LEVELNAME)
  list(APPEND Q15T_SOURCES src/boosterpool.cpp)  # the levelname companion: same option, same feature
endif()

list(TRANSFORM Q15T_SOURCES PREPEND "${Q15T_DIR}/")

set(Q15T_COMMON_OPTIONS -fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra -Werror)
set(Q15T_COMMON_LINK -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 -Wl,--exclude-libs,ALL)

add_library(nevr_q15t SHARED ${Q15T_SOURCES})
set_target_properties(nevr_q15t PROPERTIES OUTPUT_NAME q15t)
target_include_directories(nevr_q15t PRIVATE "${Q15T_DIR}/src")
target_link_libraries(nevr_q15t PRIVATE nevr::plugin_sdk dl)
target_compile_options(nevr_q15t PRIVATE ${Q15T_COMMON_OPTIONS})
target_compile_definitions(nevr_q15t PRIVATE
  Q15T_BUILD_ID="${nevr_pinned_build_id}"
  Q15T_LEVELNAME=$<BOOL:${Q15T_LEVELNAME}>)
target_link_options(nevr_q15t PRIVATE ${Q15T_COMMON_LINK})
