# The q15t Quest plugin: libq15t.so (the modules).
# Added by src/quest/integration/CMakeLists.txt, which defines nevr::plugin_sdk and nevr_pinned_build_id. The library is in no
# release or package-dev APK; the variant and combat recipes package it (src/quest/plugins/q15t/README.md). Paths are relative to this file.
set(Q15T_DIR "${CMAKE_CURRENT_LIST_DIR}")

option(Q15T_T26 "T-26: a new profile starts with the gun on the right hand, once per user id (plugin behaviour)" ON)

# ---- Gameplay and fix modules (src/<m>.{h,cpp}; docs/quest/q15t/modules/<m>.md). Defaults are each doc's own, i.e. the source build's. ----
# ON: the source build installs it unless switched off (a runtime off-file in the plugin data directory is kept where the doc keeps it).
option(Q15T_FXTRAIL "fxtrail (R29): a bullet whose trail effect has no instance ends its trail. Source: ON by default" ON)
option(Q15T_VOIPDROP "voipdrop (R30): a remote voice packet that cannot be stored is dropped. Source: DEFAULT ON" ON)
option(Q15T_CUSTCTX "custctx (R31-R33): three guards on the customization renderer. Source: DEFAULT ON" ON)
option(Q15T_GUNCONNECT "gunconnect (R17-R19): the local player's guns get Connect(local user) so they fire. Source: default ON; tick" ON)
option(Q15T_AIBOTS "aibots (R40): Practice AI bots get a scenario each combat match. Source: default ON; tick" ON)
# EC and ECFX: compiled in by default and inert until the source build's own switch file exists in the plugin data directory (ec.on,
# ec_preset1.on, ec_preset2.on, ec_preset3.on; neutronfx.on for ecfx), or Q15T_EC_DEFAULT_WORDS names words. The doc states no option default; the
# source build gates them by those files, so ON with no switch file installs no word (it publishes the zeroed re33_ec_page and logs "ec: off").
# Kill file ec.off in the data directory keeps ec off whatever else is set (module_switch.h convention).
option(Q15T_EC "ec (R43...): the EC network client, EC token page, tac-mod store/hide. Switch files in the data directory; kill file ec.off" ON)
option(Q15T_ECFX "ecfx: NEUTRON wall-effect queue drain on ec's net tick; needs Q15T_EC. Switch file neutronfx.on" ON)
set(Q15T_EC_DEFAULT_WORDS "types=VPTO equip hide" CACHE STRING "ec: the words used when no ec switch file exists (empty: ec stays off without a file). Default = the source build's latest on-list preset ec_preset3 (ec_preset3.on)")
option(Q15T_ECFX_DEFAULT_ON "ecfx: on with no neutronfx.on file (the source build's on-list ships neutronfx.on)" ON)
# TEST ONLY in the source build, but on in its wave19a on-list (droneguard, peerclamp, dronekill, dronemuzzle, hudmeter, powerboost): ON here.
# Droneguard's second stage, the levelname pair and the gate options below stay OFF (not in the on-list, or they need content in the pack).
option(Q15T_DRONEGUARD "droneguard (R35): skip a per-frame Follow call. Source: TEST ONLY; on in the wave19a on-list" ON)
option(Q15T_DRONEGUARD_IDX "droneguard stage 1 (needs Q15T_DRONEGUARD). Source: TEST ONLY" OFF)
option(Q15T_PEERCLAMP "peerclamp (R36): clamp a peer's stamp in CleanupPeers. Source: TEST ONLY; on in the wave19a on-list" ON)
option(Q15T_DRONEKILL "dronekill (R45,R47,R48): drone kill / lock-on / gun / chase / shot, all five words. Source: TEST ONLY (dronekill2.on); on in the wave19a on-list; tick" ON)
option(Q15T_DRONEMUZZLE "dronemuzzle (R49,R50): a drone's bullets leave its gun; needs Q15T_DRONEKILL. Source: TEST ONLY; on in the wave19a on-list; tick" ON)
set(Q15T_POWERBOOST 3 CACHE STRING "powerboost (R51-R56) stage: 0 off, 1 read-only, 2 swallow, 3 cone (cone needs Q15T_EC; tick). Source: TEST ONLY; the wave19a on-list ships an empty powerboost.on = swallow + cone = 3")
set_property(CACHE Q15T_POWERBOOST PROPERTY STRINGS 0 1 2 3)
option(Q15T_HUDMETER "hudmeter (R57): the wrist HUD's weapon-slot heat fill stops being 0 x 0. Source: hudmeter.on; on in the wave19a on-list" ON)
option(Q15T_LEVELNAME "levelname (R04, G7) with boosterpool: global_mp keeps its plain name, so the full-size global_mp in the pack loads. Source combat build: on. Kill file levelname.off" ON)
# shieldgamma: a bitmask, 1 = gamma (a TEST switch, off in the source), 2 = tube mirror (the ship form, on in the source).
set(Q15T_SHIELDGAMMA 2 CACHE STRING "shieldgamma (R58) bitmask: 1 gamma, 2 tube mirror, 0 not built. Source default: mirror on, gamma off")
set_property(CACHE Q15T_SHIELDGAMMA PROPERTY STRINGS 0 1 2 3)

if(Q15T_ECFX AND NOT Q15T_EC)
  message(FATAL_ERROR "Q15T_ECFX needs Q15T_EC (ecfx rides ec's recvfrom net tick)")
endif()
if(Q15T_DRONEMUZZLE AND NOT Q15T_DRONEKILL)
  message(FATAL_ERROR "Q15T_DRONEMUZZLE needs Q15T_DRONEKILL (the thunks act only inside a drone shot dronekill reports)")
endif()
if(Q15T_DRONEGUARD_IDX AND NOT Q15T_DRONEGUARD)
  message(FATAL_ERROR "Q15T_DRONEGUARD_IDX needs Q15T_DRONEGUARD")
endif()
if(NOT Q15T_POWERBOOST MATCHES "^[0-3]$")
  message(FATAL_ERROR "Q15T_POWERBOOST must be 0, 1, 2 or 3 (got '${Q15T_POWERBOOST}')")
endif()
if(NOT Q15T_SHIELDGAMMA MATCHES "^[0-3]$")
  message(FATAL_ERROR "Q15T_SHIELDGAMMA must be 0, 1, 2 or 3 (got '${Q15T_SHIELDGAMMA}')")
endif()
if(Q15T_POWERBOOST EQUAL 3 AND NOT Q15T_EC)
  message(WARNING "Q15T_POWERBOOST=3 without Q15T_EC: there is no net tick, the cone stage is refused at run time and stage 2 stays")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(Q15T_SOURCES src/plugin.cpp src/plugin_log.cpp src/script_redirect.cpp src/import_hook.cpp src/combat_map.cpp src/game_tick.cpp src/safe_read.cpp src/got_swap.cpp src/weapon_arm.cpp src/gamespace.cpp)
foreach(m FXTRAIL VOIPDROP CUSTCTX GUNCONNECT AIBOTS EC ECFX DRONEGUARD PEERCLAMP DRONEKILL DRONEMUZZLE HUDMETER LEVELNAME)
  if(Q15T_${m})
    string(TOLOWER ${m} lm)
    list(APPEND Q15T_SOURCES src/${lm}.cpp)
  endif()
endforeach()
if(Q15T_POWERBOOST GREATER 0)
  list(APPEND Q15T_SOURCES src/powerboost.cpp)
endif()
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
  Q15T_T26=$<BOOL:${Q15T_T26}>
  Q15T_FXTRAIL=$<BOOL:${Q15T_FXTRAIL}> Q15T_VOIPDROP=$<BOOL:${Q15T_VOIPDROP}> Q15T_CUSTCTX=$<BOOL:${Q15T_CUSTCTX}>
  Q15T_GUNCONNECT=$<BOOL:${Q15T_GUNCONNECT}> Q15T_AIBOTS=$<BOOL:${Q15T_AIBOTS}> Q15T_EC=$<BOOL:${Q15T_EC}> Q15T_ECFX=$<BOOL:${Q15T_ECFX}>
  Q15T_DRONEGUARD=$<BOOL:${Q15T_DRONEGUARD}> Q15T_DRONEGUARD_IDX=$<BOOL:${Q15T_DRONEGUARD_IDX}> Q15T_PEERCLAMP=$<BOOL:${Q15T_PEERCLAMP}>
  Q15T_DRONEKILL=$<BOOL:${Q15T_DRONEKILL}> Q15T_DRONEMUZZLE=$<BOOL:${Q15T_DRONEMUZZLE}> Q15T_POWERBOOST=${Q15T_POWERBOOST}
  Q15T_HUDMETER=$<BOOL:${Q15T_HUDMETER}> Q15T_LEVELNAME=$<BOOL:${Q15T_LEVELNAME}> Q15T_SHIELDGAMMA=${Q15T_SHIELDGAMMA}
  Q15T_ECFX_DEFAULT_ON=$<BOOL:${Q15T_ECFX_DEFAULT_ON}>)
if(NOT "${Q15T_EC_DEFAULT_WORDS}" STREQUAL "")
  target_compile_definitions(nevr_q15t PRIVATE "Q15T_EC_DEFAULT_WORDS=\"${Q15T_EC_DEFAULT_WORDS}\"")
endif()
target_link_options(nevr_q15t PRIVATE ${Q15T_COMMON_LINK})
