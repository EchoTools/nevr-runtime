# The q15t Quest plugin: libq15t.so (the modules).
# Added by src/quest/integration/CMakeLists.txt, which defines nevr::plugin_sdk and nevr_pinned_build_id. The library is in no
# release or package-dev APK; the variant and combat recipes package it (src/quest/plugins/q15t/README.md). Paths are relative to this file.
set(Q15T_DIR "${CMAKE_CURRENT_LIST_DIR}")

# ---- Gameplay and fix modules (src/<m>.{h,cpp}; docs/quest/q15t/modules/<m>.md). Defaults are each doc's own, i.e. the source build's. ----
# ON: the source build installs it unless switched off (a runtime off-file in the plugin data directory is kept where the doc keeps it).
option(Q15T_FXTRAIL "fxtrail (R29): a bullet whose trail effect has no instance ends its trail. Source: ON by default" ON)
option(Q15T_VOIPDROP "voipdrop (R30): a remote voice packet that cannot be stored is dropped. Source: DEFAULT ON" ON)
option(Q15T_CUSTCTX "custctx (R31-R33): three guards on the customization renderer. Source: DEFAULT ON" ON)
# The runtime owns impactsnd and decals (#2183); ec, ecfx, POWER BOOST, dronekill and dronemuzzle (#2187); gunconnect, aibots and weapon_arm (#2185):
# src/quest/combat/, keys combat.impactsnd, combat.impactsnd_write, combat.decals, combat.decals_flip_u, combat.decals_flip_v, combat.ec,
# combat.ec_words, combat.ecfx, combat.powerboost_stage, combat.dronekill, combat.dronemuzzle, combat.gunconnect, combat.aibots, combat.aibots_scenario, combat.weapon_arm.
# TEST ONLY in the source build, but on in its wave19a on-list (droneguard, peerclamp, hudmeter): ON here.
# Droneguard's second stage, the levelname pair and the gate options below stay OFF (not in the on-list, or they need content in the pack).
option(Q15T_DRONEGUARD "droneguard (R35): skip a per-frame Follow call. Source: TEST ONLY; on in the wave19a on-list" ON)
option(Q15T_DRONEGUARD_IDX "droneguard stage 1 (needs Q15T_DRONEGUARD). Source: TEST ONLY" OFF)
option(Q15T_PEERCLAMP "peerclamp (R36): clamp a peer's stamp in CleanupPeers. Source: TEST ONLY; on in the wave19a on-list" ON)
option(Q15T_HUDMETER "hudmeter (R57): the wrist HUD's weapon-slot heat fill stops being 0 x 0. Source: hudmeter.on; on in the wave19a on-list" ON)
option(Q15T_LEVELNAME "levelname (R04, G7) with boosterpool: global_mp keeps its plain name, so the full-size global_mp in the pack loads. Source combat build: on. Kill file levelname.off" ON)
# shieldgamma: a bitmask, 1 = gamma (a TEST switch, off in the source), 2 = tube mirror (the ship form, on in the source).
set(Q15T_SHIELDGAMMA 2 CACHE STRING "shieldgamma (R58) bitmask: 1 gamma, 2 tube mirror, 0 not built. Source default: mirror on, gamma off")
set_property(CACHE Q15T_SHIELDGAMMA PROPERTY STRINGS 0 1 2 3)

if(Q15T_DRONEGUARD_IDX AND NOT Q15T_DRONEGUARD)
  message(FATAL_ERROR "Q15T_DRONEGUARD_IDX needs Q15T_DRONEGUARD")
endif()
if(NOT Q15T_SHIELDGAMMA MATCHES "^[0-3]$")
  message(FATAL_ERROR "Q15T_SHIELDGAMMA must be 0, 1, 2 or 3 (got '${Q15T_SHIELDGAMMA}')")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(Q15T_SOURCES src/plugin.cpp src/plugin_log.cpp src/script_redirect.cpp src/import_hook.cpp src/combat_map.cpp src/game_tick.cpp src/safe_read.cpp src/got_swap.cpp src/gamespace.cpp)
foreach(m FXTRAIL VOIPDROP CUSTCTX DRONEGUARD PEERCLAMP HUDMETER LEVELNAME)
  if(Q15T_${m})
    string(TOLOWER ${m} lm)
    list(APPEND Q15T_SOURCES src/${lm}.cpp)
  endif()
endforeach()
if(Q15T_SHIELDGAMMA GREATER 0)
  list(APPEND Q15T_SOURCES src/shieldgamma.cpp)
endif()
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
  Q15T_FXTRAIL=$<BOOL:${Q15T_FXTRAIL}> Q15T_VOIPDROP=$<BOOL:${Q15T_VOIPDROP}> Q15T_CUSTCTX=$<BOOL:${Q15T_CUSTCTX}>
  Q15T_DRONEGUARD=$<BOOL:${Q15T_DRONEGUARD}> Q15T_DRONEGUARD_IDX=$<BOOL:${Q15T_DRONEGUARD_IDX}> Q15T_PEERCLAMP=$<BOOL:${Q15T_PEERCLAMP}>
  Q15T_HUDMETER=$<BOOL:${Q15T_HUDMETER}> Q15T_LEVELNAME=$<BOOL:${Q15T_LEVELNAME}> Q15T_SHIELDGAMMA=${Q15T_SHIELDGAMMA})
target_link_options(nevr_q15t PRIVATE ${Q15T_COMMON_LINK})
