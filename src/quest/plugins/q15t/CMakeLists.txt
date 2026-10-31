# The q15t Quest plugin: libq15t.so (the modules) and libq15t_early.so (heaps and memsys, for the loader's early phase).
# Added by src/quest/integration/CMakeLists.txt, which defines nevr::plugin_sdk and nevr_pinned_build_id. Neither library is in a
# release or package-dev APK; the variant recipe packages them (src/quest/plugins/q15t/README.md). Paths are relative to this file.
set(Q15T_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The 21 script libraries whose headset-type expression decides a combat entry on a Quest: the 20 of the combat menu (the combat
# buttons, FIND COMBAT, the combat mode card, the level select pages, the "combat allowed" prefab) and the lobby script
# (libf808c072bb49da47.so), whose two LoadLevel branch nodes read the expression's quest output to pick the lobby wing that is
# loaded (a Quest gets the arena wing only; the Link answer takes the combat wing's pin). The menu gate answers only these. Every
# other script that binds the expression (47 libraries in all) gets the real answer. Comma separated file names.
set(Q15T_LINK_GATE_LIBS "lib555db38933fef859.so,lib1da85b18cbad6297.so,lib4e984de06cafe2ab.so,lib2d5ebd0e1895f269.so,libcab86dd5921bafc7.so,lib5e311be23be97835.so,libbad7ce9b6c6df2cb.so,libf36316e246f2ca21.so,lib8325751e75df5860.so,lib18000621255c6ea5.so,lib1f28bd4fa7d724fb.so,lib3ded824fccc2bf6d.so,lib5ddc3dcdad4a3ed5.so,lib8515ab072bc095a8.so,libb56c58de2b68fc61.so,libb7882b9d804f86c1.so,libb8bd659eb6ed3927.so,libbc170a382fda5499.so,libd280bb8333ad45ed.so,libfb4ad3853db57943.so,libf808c072bb49da47.so" CACHE STRING "Menu gate: the script libraries (file names, comma separated; 'all'; 'none') whose headset-type expression is told no kind of headset. A gate_libs.txt in the plugin data directory overrides it")
option(Q15T_GATE_PERM "Gameplay (G1): the party-permissions expression stops refusing combat to a Quest headset. Source combat build: on. Kill file gate_perm.off" ON)
option(Q15T_GATE_LINK "Menu gate: the headset-type expression is told a Link headset (the PC build's kind 4, not the Quest kind), so the menu scripts stop locking combat cards to a Quest (a caller-range answer, no code write)" ON)
option(Q15T_GATE_BOTS "Gameplay (G6): the bot-body chooser stops picking the arena bot for a Quest headset. Installed only with levelname (the combat bot body is in the full global_mp). Source combat build: on. Kill file gate_bots.off" ON)
option(Q15T_T26 "T-26: a new profile starts with the gun on the right hand, once per user id (plugin behaviour)" ON)
option(Q15T_HZ_MATCH90 "Hz level rule: 90 Hz (72 on a Quest 2) while a combat map or the social lobby is loaded, the game's own rate in Echo Arena and elsewhere (q15t hzlobby90 + hz72q2, on in its shipped list). Kill files hzlevel.off, hzlobby90.off, hz72q2.off" ON)
set(Q15T_CPU_LEVEL 4 CACHE STRING "CPU level the clock raise (args {clock: raise}) asks for at least")
set(Q15T_GPU_LEVEL 4 CACHE STRING "GPU level the clock raise (args {clock: raise}) asks for at least")

# ---- Gameplay and fix modules (src/<m>.{h,cpp}; docs/quest/q15t/modules/<m>.md). Defaults are each doc's own, i.e. the source build's. ----
# ON: the source build installs it unless switched off (a runtime off-file in the plugin data directory is kept where the doc keeps it).
option(Q15T_FXTRAIL "fxtrail (R29): a bullet whose trail effect has no instance ends its trail. Source: ON by default" ON)
option(Q15T_VOIPDROP "voipdrop (R30): a remote voice packet that cannot be stored is dropped. Source: DEFAULT ON" ON)
option(Q15T_CUSTCTX "custctx (R31-R33): three guards on the customization renderer. Source: DEFAULT ON" ON)
option(Q15T_MESHSTEP "meshstep (#1555): CGMeshListResource tables grow by step 1 (reconstructed constructor, GOT word). Kill file meshstep.off" ON)
option(Q15T_GUNCONNECT "gunconnect (R17-R19): the local player's guns get Connect(local user) so they fire. Source: default ON; tick" ON)
option(Q15T_AIBOTS "aibots (R40): Practice AI bots get a scenario each combat match. Source: default ON; tick" ON)
option(Q15T_BVH "bvh (R20-R25): a real static raycast tree for CBVHTree. Source: on by default (kill file bvh.off); tick" ON)
option(Q15T_WALLFIRE "wallfire (R26-R28): no shot through a wall; needs Q15T_BVH. Source: on by default (kill file wallfire.off)" ON)
option(Q15T_IMPACTSND "impactsnd (R12-R15): the impact table gets its Wwise ids. Source: default ON since v6; tick" ON)
# EC and ECFX: compiled in by default and inert until the source build's own switch file exists in the plugin data directory (ec.on,
# ec_preset1.on, ec_preset2.on, ec_preset3.on; neutronfx.on for ecfx), or Q15T_EC_DEFAULT_WORDS names words. The doc states no option default; the
# source build gates them by those files, so ON with no switch file installs no word (it publishes the zeroed re33_ec_page and logs "ec: off").
# Kill file ec.off in the data directory keeps ec off whatever else is set (module_switch.h convention, as decals.off / pfx.off).
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
option(Q15T_MUSIC "music (T-48): the combat music is stopped when a match ends or a new one starts, so instances do not stack. Source: default ON; tick" ON)
option(Q15T_SENSORWD "sensorwd (T-07): a scanner sphere still enabled 6 s after its enable is turned off again. Source: default ON (on-list); tick" ON)
# shaderstep (issue #1553): the CGShaderSetResource table growth step is 1, set when the factory constructs the object. Off: shaderstep.off.
option(Q15T_SHADERSTEP "shaderstep (#1553): CGShaderSetResource table growth step 32 -> 1 in the factory constructor (vtable wrapper, no code write)" ON)
option(Q15T_LEVELNAME "levelname (R04, G7) with boosterpool: global_mp keeps its plain name, so the full-size global_mp in the pack loads. Source combat build: on. Kill file levelname.off" ON)
option(Q15T_INSTMODEL "instmodel (disassembly 0x1a078dc): CGInstancedModelResource's constructor with the growth step of its CTableXT members at 1, not 32 (the word row this replaces: a code write). Source: default ON; no tick" ON)
# pfx (issue #1631): the particle-effect renderer's gate, read by every pfx* module. Kill file pfx.off in the data directory. The working build has
# the renderer on, so ON here; the source build's On() is off unless pfx.on exists (the renderer itself is not ported yet: docs/quest/q15t/modules/pfx.md).
option(Q15T_PFX "pfx (#1631): the particle-effect renderer's gate, the switch every pfx* module reads. Working build: on. Kill file pfx.off" ON)
# shieldgamma: a bitmask, 1 = gamma (a TEST switch, off in the source), 2 = tube mirror (the ship form, on in the source).
set(Q15T_SHIELDGAMMA 2 CACHE STRING "shieldgamma (R58) bitmask: 1 gamma, 2 tube mirror, 0 not built. Source default: mirror on, gamma off")
set_property(CACHE Q15T_SHIELDGAMMA PROPERTY STRINGS 0 1 2 3)
option(Q15T_IMPACTSND_NOWRITE "impactsnd: read and log only, no row is written" OFF)
# tablestep (issue #1552): the shader-set resource table's growth step 32 -> 1 (CFactory<CGShaderSetResource>::CreateInstance, disassembly
# libr15 0x1a0707c, a wrapper on its three data slots). Source: the shipped variant's word row (extra_rows: 0) is not in the APK, so
# this is the plugin's own. Off file: tablestep.off in the plugin data directory.
option(Q15T_TABLESTEP "tablestep (#1552): shader-set table growth step 32 -> 1 by a wrapper on CreateInstance's data slots. Source: ON" ON)
# sendguard: CSendEventNode::Enter (disassembly libr15 0x2825a80) faults on a send to a game space that is not loaded (the menu gate lets the menu
# reach such a send on a Quest without the combat content pack). A wrapper on its data slots drops that send with a log line. Off file: sendguard.off.
option(Q15T_SENDGUARD "sendguard: a script send to a game space that is not loaded is dropped, not faulted, by a wrapper on CSendEventNode::Enter's data slots" ON)
# decals (issue #1619): the combat levels' static decals, which the Quest renderer cannot draw, as quads in dynamic-mesh nodes from the
# content pack's decals.bin. ON, and inert (one INFO line, reason "off") when no decals.bin is found. Off file: decals.off. Needs impactsnd's
# Shutdown wrapper to be installed first when Q15T_IMPACTSND is on (plugin.cpp installs it in that order; the wrapper chains on it).
# pfxtev (issue #1642): the six event-effect ids may be created. The decision only: the renderer that calls it is pfx (#1631), not in
# this tree yet. Kill file pfxtev.off in the plugin data directory.
option(Q15T_PFXTEV "pfxtev (#1642): the event-effect id decision (PfxTevAllows); no caller until the pfx renderer (#1631) lands. Source: ON" ON)
option(Q15T_DECALS "decals (#1619): the level decals of decals.bin drawn as dynamic-mesh quads on the bullet system's update, torn down on its Shutdown" ON)
# heaps and memsys (docs/quest/q15t/modules/heaps.md section 1, memsys.md) need the loader's early phase (config.yaml `early: true`), and one plugin
# entry is one phase, so they are a second library, libq15t_early.so (src/plugin_early.cpp), listed as its own early entry. Whether the
# early phase runs on a headset is ADR 0019 P10; the loader refuses the entry loudly (early_unsupported) when it does not.
option(Q15T_EARLY "libq15t_early.so: heaps and memsys, for the loader's early phase" ON)

if(Q15T_WALLFIRE AND NOT Q15T_BVH)
  message(FATAL_ERROR "Q15T_WALLFIRE needs Q15T_BVH (wallfire refuses without the static BVH answering Occluded)")
endif()
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

set(Q15T_SOURCES src/plugin.cpp src/plugin_log.cpp src/import_hook.cpp src/clock_levels.cpp src/plugin_args.cpp src/refresh_rate.cpp src/game_tick.cpp src/safe_read.cpp src/headset_gates.cpp src/got_swap.cpp src/weapon_arm.cpp src/gamespace.cpp)
foreach(m FXTRAIL VOIPDROP CUSTCTX MESHSTEP GUNCONNECT AIBOTS BVH WALLFIRE IMPACTSND EC ECFX DRONEGUARD PEERCLAMP DRONEKILL DRONEMUZZLE HUDMETER LEVELNAME MUSIC SENSORWD INSTMODEL DECALS PFX PFXTEV)
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
if(Q15T_TABLESTEP)
  list(APPEND Q15T_SOURCES src/tablestep.cpp)
endif()
if(Q15T_SENDGUARD)
  list(APPEND Q15T_SOURCES src/sendguard.cpp)
endif()
if(Q15T_SHADERSTEP)
  list(APPEND Q15T_SOURCES src/shaderstep.cpp)
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
  "Q15T_LINK_GATE_LIBS=\"${Q15T_LINK_GATE_LIBS}\""
  Q15T_GATE_LINK=$<BOOL:${Q15T_GATE_LINK}> Q15T_GATE_PERM=$<BOOL:${Q15T_GATE_PERM}> Q15T_GATE_BOTS=$<BOOL:${Q15T_GATE_BOTS}>
  Q15T_HZ_MATCH90=$<BOOL:${Q15T_HZ_MATCH90}> Q15T_T26=$<BOOL:${Q15T_T26}> Q15T_CPU_LEVEL=${Q15T_CPU_LEVEL} Q15T_GPU_LEVEL=${Q15T_GPU_LEVEL}
  Q15T_FXTRAIL=$<BOOL:${Q15T_FXTRAIL}> Q15T_VOIPDROP=$<BOOL:${Q15T_VOIPDROP}> Q15T_CUSTCTX=$<BOOL:${Q15T_CUSTCTX}> Q15T_MESHSTEP=$<BOOL:${Q15T_MESHSTEP}>
  Q15T_GUNCONNECT=$<BOOL:${Q15T_GUNCONNECT}> Q15T_AIBOTS=$<BOOL:${Q15T_AIBOTS}> Q15T_BVH=$<BOOL:${Q15T_BVH}>
  Q15T_WALLFIRE=$<BOOL:${Q15T_WALLFIRE}> Q15T_IMPACTSND=$<BOOL:${Q15T_IMPACTSND}> Q15T_EC=$<BOOL:${Q15T_EC}> Q15T_ECFX=$<BOOL:${Q15T_ECFX}>
  Q15T_DRONEGUARD=$<BOOL:${Q15T_DRONEGUARD}> Q15T_DRONEGUARD_IDX=$<BOOL:${Q15T_DRONEGUARD_IDX}> Q15T_PEERCLAMP=$<BOOL:${Q15T_PEERCLAMP}>
  Q15T_DRONEKILL=$<BOOL:${Q15T_DRONEKILL}> Q15T_DRONEMUZZLE=$<BOOL:${Q15T_DRONEMUZZLE}> Q15T_POWERBOOST=${Q15T_POWERBOOST}
  Q15T_HUDMETER=$<BOOL:${Q15T_HUDMETER}> Q15T_LEVELNAME=$<BOOL:${Q15T_LEVELNAME}> Q15T_MUSIC=$<BOOL:${Q15T_MUSIC}> Q15T_SENSORWD=$<BOOL:${Q15T_SENSORWD}> Q15T_SHIELDGAMMA=${Q15T_SHIELDGAMMA}
  Q15T_IMPACTSND_NOWRITE=$<BOOL:${Q15T_IMPACTSND_NOWRITE}> Q15T_ECFX_DEFAULT_ON=$<BOOL:${Q15T_ECFX_DEFAULT_ON}>
  Q15T_SHADERSTEP=$<BOOL:${Q15T_SHADERSTEP}> Q15T_TABLESTEP=$<BOOL:${Q15T_TABLESTEP}> Q15T_SENDGUARD=$<BOOL:${Q15T_SENDGUARD}> Q15T_INSTMODEL=$<BOOL:${Q15T_INSTMODEL}> Q15T_DECALS=$<BOOL:${Q15T_DECALS}> Q15T_PFX=$<BOOL:${Q15T_PFX}>
  Q15T_PFXTEV=$<BOOL:${Q15T_PFXTEV}>)
if(NOT "${Q15T_EC_DEFAULT_WORDS}" STREQUAL "")
  target_compile_definitions(nevr_q15t PRIVATE "Q15T_EC_DEFAULT_WORDS=\"${Q15T_EC_DEFAULT_WORDS}\"")
endif()
target_link_options(nevr_q15t PRIVATE ${Q15T_COMMON_LINK})

if(Q15T_EARLY)
  add_library(nevr_q15t_early SHARED
    "${Q15T_DIR}/src/plugin_early.cpp" "${Q15T_DIR}/src/plugin_log.cpp" "${Q15T_DIR}/src/got_swap.cpp" "${Q15T_DIR}/src/safe_read.cpp"
    "${Q15T_DIR}/src/import_hook.cpp" "${Q15T_DIR}/src/heaps.cpp" "${Q15T_DIR}/src/memsys.cpp")
  set_target_properties(nevr_q15t_early PROPERTIES OUTPUT_NAME q15t_early)
  target_include_directories(nevr_q15t_early PRIVATE "${Q15T_DIR}/src")
  target_link_libraries(nevr_q15t_early PRIVATE nevr::plugin_sdk dl)
  target_compile_options(nevr_q15t_early PRIVATE ${Q15T_COMMON_OPTIONS})
  target_compile_definitions(nevr_q15t_early PRIVATE Q15T_BUILD_ID="${nevr_pinned_build_id}")
  target_link_options(nevr_q15t_early PRIVATE ${Q15T_COMMON_LINK})
endif()
