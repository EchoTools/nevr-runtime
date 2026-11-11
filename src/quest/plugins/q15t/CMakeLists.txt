# The q15t Quest plugin: libq15t.so (the modules).
# Added by src/quest/integration/CMakeLists.txt, which defines nevr::plugin_sdk and nevr_pinned_build_id. The library is in no
# release or package-dev APK; the variant and combat recipes package it (src/quest/plugins/q15t/README.md). Paths are relative to this file.
set(Q15T_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ---- Gameplay and fix modules: none are left in this plugin. ----
# The runtime owns fxtrail, voipdrop, custctx, droneguard and peerclamp (#2178); hudmeter and shieldgamma (#2186); impactsnd and decals (#2183); ec, ecfx, POWER BOOST, dronekill and dronemuzzle (#2187); gunconnect, aibots and weapon_arm (#2185); levelname and boosterpool (#2181):
# src/quest/combat/, keys combat.impactsnd, combat.impactsnd_write, combat.decals, combat.decals_flip_u, combat.decals_flip_v, combat.ec,
# combat.ec_words, combat.ecfx, combat.powerboost_stage, combat.dronekill, combat.dronemuzzle, combat.gunconnect, combat.aibots, combat.aibots_scenario, combat.weapon_arm, combat.hudmeter, combat.shieldgamma_gamma,
# combat.shieldgamma_tube_mirror, combat.fxtrail, combat.voipdrop, combat.custctx, combat.droneguard, combat.droneguard_stage1, combat.peerclamp, combat.levelname.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(Q15T_SOURCES src/plugin.cpp src/plugin_log.cpp src/script_redirect.cpp src/import_hook.cpp src/combat_map.cpp src/game_tick.cpp src/safe_read.cpp src/got_swap.cpp src/gamespace.cpp)

list(TRANSFORM Q15T_SOURCES PREPEND "${Q15T_DIR}/")

set(Q15T_COMMON_OPTIONS -fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra -Werror)
set(Q15T_COMMON_LINK -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 -Wl,--exclude-libs,ALL)

add_library(nevr_q15t SHARED ${Q15T_SOURCES})
set_target_properties(nevr_q15t PROPERTIES OUTPUT_NAME q15t)
target_include_directories(nevr_q15t PRIVATE "${Q15T_DIR}/src")
target_link_libraries(nevr_q15t PRIVATE nevr::plugin_sdk dl)
target_compile_options(nevr_q15t PRIVATE ${Q15T_COMMON_OPTIONS})
target_compile_definitions(nevr_q15t PRIVATE
  Q15T_BUILD_ID="${nevr_pinned_build_id}")
target_link_options(nevr_q15t PRIVATE ${Q15T_COMMON_LINK})
