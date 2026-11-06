# The sentinel's integration: the constructor sequence, the dlopen hook and post-load installs, the
# token-auth identity source, the bridge with its frame tap, and the production wiring that binds them to
# the library packages (docs/adr/0003, "Integration").
#
# Translation units that include callback_thunk.h (dlopen_hook.cpp, social_shim.cpp) are built without
# exceptions; everything else keeps them. NEVR_OUTSIDE_GAME_CALL marks the functions the hook handlers call
# directly that run outside any game call (outside_game_call.h).

execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse HEAD
                OUTPUT_VARIABLE NEVR_QUEST_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" describe --tags --always --dirty
                OUTPUT_VARIABLE NEVR_QUEST_GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT NEVR_QUEST_GIT_COMMIT)
  set(NEVR_QUEST_GIT_COMMIT "unknown")
endif()
if(NOT NEVR_QUEST_GIT_DESCRIBE)
  set(NEVR_QUEST_GIT_DESCRIBE "unknown")
endif()

# The Quest plugin loader (docs/adr/0019-quest-plugin-loader.md): the plan, the ELF checks, the loader, its records and the
# sentinel's binding of it. The PC's plan builder (runtime/ext/plugin_load_plan.cpp) is compiled in so one `plugins:` list
# reads the same on both platforms. plugin_game_image.cpp and input_seam_hook.cpp reach pinned_targets.h, and slot_hook.cpp
# installs the handlers the game calls, so all three are built without exceptions.
add_library(nevr_quest_plugin STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/packaged_plugins.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_plan.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_loader.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_record_log.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_platform.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_runtime.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_game_image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/input_seam_hook.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/script_redirect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/slot_hook.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ext/plugin_load_plan.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_game_image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/input_seam_hook.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/script_redirect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/slot_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_include_directories(nevr_quest_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_include_directories(nevr_quest_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel")
target_link_libraries(nevr_quest_plugin PUBLIC nevr::plugin_sdk nevr_quest_config nevr_quest_got_hook nevr_quest_data nlohmann_json::nlohmann_json)
# Hidden visibility: linked into the sentinel, nothing here may become an export (tests/quest TestExportAllowlist).
target_compile_options(nevr_quest_plugin PRIVATE -Wall -Wextra -Werror -fvisibility=hidden -fvisibility-inlines-hidden)

add_library(nevr_quest_integration STATIC
  ctor_sequence.cpp
  ../sentinel/build_variant.cpp
  libr15_attach_step.cpp
  post_load.cpp
  identity_source.cpp
  stage_log.cpp
  bridge_uri.cpp
  production_steps.cpp
  crash_send_step.cpp
  self_check_wiring.cpp
  dlopen_hook.cpp
  social_shim.cpp)
set_source_files_properties(dlopen_hook.cpp social_shim.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_include_directories(nevr_quest_integration PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_include_directories(nevr_quest_integration PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel")
target_link_libraries(nevr_quest_integration PUBLIC
  nevr_quest_plugin
  nevr_quest_config
  nevr_quest_got_hook
  nevr_quest_redirect
  nevr_quest_login
  nevr_quest_social
  nevr_quest_self_check
  nevr_quest_net
  nevr_quest_token_auth
  nevr_quest_hwdump
  nevr_quest_invite
  nlohmann_json::nlohmann_json)
# Hidden visibility: linked into the sentinel, nothing here may become an export
# (tests/quest TestExportAllowlist).
target_compile_options(nevr_quest_integration PRIVATE -Wall -Wextra -Werror
  -fvisibility=hidden -fvisibility-inlines-hidden)
set_source_files_properties(production_steps.cpp PROPERTIES COMPILE_DEFINITIONS
  "NEVR_QUEST_GIT_COMMIT=\"${NEVR_QUEST_GIT_COMMIT}\";NEVR_QUEST_GIT_DESCRIBE=\"${NEVR_QUEST_GIT_DESCRIBE}\";NEVR_QUEST_PROJECT_VERSION=\"${NEVR_QUEST_GIT_DESCRIBE}\";NEVR_QUEST_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")

# Android-linkable builds of the host tests (they cannot execute on the build machine); the same
# sources run on the host in `just test-quest-integration`.
foreach(test_name integration_sequence_test integration_hooks_test)
  add_executable(${test_name} "${CMAKE_CURRENT_SOURCE_DIR}/../tests/${test_name}.cpp")
  target_link_libraries(${test_name} PRIVATE nevr_quest_integration)
  target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel")
  target_compile_options(${test_name} PRIVATE -Wall -Wextra -Werror)
endforeach()
# The counter budget in integration_hooks_test registers the #239 login-prompt hook's counters through its
# real function. The hook's translation unit includes callback_thunk.h, so it is built without exceptions
# here as in the sentinel (sentinel/CMakeLists.txt).
target_sources(integration_hooks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/login_prompt_hook.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/login_prompt_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
# The frame dispatcher (#966 S7) the Update handler calls: the same hook path, so built without exceptions too.
target_sources(integration_hooks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../invite/frame_dispatch.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../invite/frame_dispatch.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_link_libraries(integration_hooks_test PRIVATE nevr_quest_prompt_board)
# And the #1137 login title hook's (registered with the login prompt's, in the same step).
target_sources(integration_hooks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/login_title_hook.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/login_title_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
# The same test registers the #319 OBB-skip hook's counters through its real function (also without exceptions).
target_sources(integration_hooks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/obb_skip_hook.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/obb_skip_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
# And the #871 resource request hook's counters (also without exceptions).
target_sources(integration_hooks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/resource_request_hook.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/resource_request_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
# And the #943 texture-quality hook's counters (also without exceptions).
target_sources(integration_hooks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/texture_quality_hook.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/texture_quality_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
# And the #1012 Texture Resolution control's counters (the hook without exceptions; the pure rewrite it calls keeps them).
target_sources(integration_hooks_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/texture_menu_hook.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/texture_menu.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/texture_menu_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_link_libraries(integration_hooks_test PRIVATE nlohmann_json::nlohmann_json)
# And the #928 clock-level override's counters (also without exceptions).
target_sources(integration_hooks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/clock_levels_hook.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/clock_levels_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")

# A no-op plugin for a headset run of the loader (docs/adr/0019, slice 1): the host test's fixture, named `nevr_noop` and
# declaring the libr15 build the sentinel is pinned to. It is not part of the sentinel or of any APK the recipes build; a
# development APK gets it from `just android-repack-apk ... plugin=<this file>`.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/pinned_targets.h" nevr_pinned_build_id_line
     REGEX "kLibR15StockBuildId = \"[0-9a-f]+\"")
string(REGEX MATCH "\"([0-9a-f]+)\"" nevr_pinned_build_id_match "${nevr_pinned_build_id_line}")
set(nevr_pinned_build_id "${CMAKE_MATCH_1}")
if(NOT nevr_pinned_build_id)
  message(FATAL_ERROR "src/quest/sentinel/pinned_targets.h has no kLibR15StockBuildId literal")
endif()
add_library(nevr_noop_plugin SHARED "${CMAKE_CURRENT_SOURCE_DIR}/../tests/plugin_fixture.cpp")
set_target_properties(nevr_noop_plugin PROPERTIES OUTPUT_NAME nevr_noop)
target_compile_definitions(nevr_noop_plugin PRIVATE FX_NAME="nevr_noop" FX_IDS="${nevr_pinned_build_id}" FX_NO_CTOR)
target_include_directories(nevr_noop_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(nevr_noop_plugin PRIVATE nevr::plugin_sdk)
target_compile_options(nevr_noop_plugin PRIVATE -Wall -Wextra -Werror)
target_link_options(nevr_noop_plugin PRIVATE -Wl,--build-id=sha1)

# The autopilot (#1202, docs/adr/0025-quest-autopilot-route.md): a development-build plugin that drives the controllers
# through a route file, for repeatable performance runs. It declares the libr15 build the input seam is pinned to. Like
# nevr_noop it is in no APK the release or package-dev recipes build; `just quest-autopilot-apk` carries it. Built without
# exceptions (its filter runs on the game's input thread); hidden visibility, so only the NEVR_PLUGIN_API entry points are
# exported.
add_library(nevr_autopilot_plugin SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/autopilot/plugin.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/autopilot/autopilot.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/autopilot/route.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../data/sha256.cpp")
set_target_properties(nevr_autopilot_plugin PROPERTIES OUTPUT_NAME nevr_autopilot)
target_compile_definitions(nevr_autopilot_plugin PRIVATE NEVR_AUTOPILOT_BUILD_IDS="${nevr_pinned_build_id}")
target_include_directories(nevr_autopilot_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(nevr_autopilot_plugin PRIVATE nevr::plugin_sdk nlohmann_json::nlohmann_json)
target_compile_options(nevr_autopilot_plugin PRIVATE -Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden
                       -fvisibility-inlines-hidden)
# --exclude-libs: the static libc++ archive keeps its symbols out of the export set, as the sentinel does.
target_link_options(nevr_autopilot_plugin PRIVATE -Wl,--build-id=sha1 -Wl,--exclude-libs,ALL)

# The q15t plugin (#1546): libq15t.so, built here and carried by no APK the release or package-dev recipes build;
# `just build-quest-q15t` builds it, and the variant and combat recipes package it.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../plugins/q15t" "${CMAKE_CURRENT_BINARY_DIR}/q15t")

# Sensor controls, never run: tests/quest frames_sensor_test.go reads them. Both hooks have a handler that
# calls a helper with a personality directly; only in frames_probe_ok is the helper annotated.
foreach(variant ok bad)
  add_executable(frames_probe_${variant}
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_probe_handler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_probe_helper_${variant}.cpp")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_probe_handler.cpp"
                              PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
  target_include_directories(frames_probe_${variant} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel" "${CMAKE_CURRENT_SOURCE_DIR}/../tests" "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  target_link_libraries(frames_probe_${variant} PRIVATE nevr_quest_got_hook)
  target_compile_options(frames_probe_${variant} PRIVATE -Wall -Wextra -Werror)
endforeach()

# The same controls for a reviewed assembly entry (sentinel/callback_thunk.h rule 7): the entry's record names the entry and the
# C++ continuation the sensor walks. In ok the entry's CFI describes its frame and the helper is annotated; in bad the helper is
# live across the call; in nocfi the entry has no FDE; in naked it is an __attribute__((naked)) function, whose FDE does not
# describe its frame; in ldr it reads its words with a plain load.
foreach(variant ok bad nocfi naked ldr)
  set(_helper ok)
  if(variant STREQUAL "bad")
    set(_helper bad)
  endif()
  add_executable(frames_asm_probe_${variant}
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_probe_asm.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_probe_helper_${_helper}.cpp")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_probe_asm.cpp"
                              PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
  if(variant STREQUAL "nocfi")
    target_compile_definitions(frames_asm_probe_nocfi PRIVATE PROBE_NO_CFI=1)
  elseif(variant STREQUAL "naked")
    target_compile_definitions(frames_asm_probe_naked PRIVATE PROBE_NAKED=1)
  elseif(variant STREQUAL "ldr")
    target_compile_definitions(frames_asm_probe_ldr PRIVATE PROBE_PLAIN_LDR=1)
  endif()
  target_include_directories(frames_asm_probe_${variant} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel" "${CMAKE_CURRENT_SOURCE_DIR}/../tests" "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  target_link_libraries(frames_asm_probe_${variant} PRIVATE nevr_quest_got_hook)
  target_compile_options(frames_asm_probe_${variant} PRIVATE -Wall -Wextra -Werror)
endforeach()

# The assembly-entry check's other controls (tests/quest frames_sensor_test.go, frames_asm_probes_test.go). Each links the ok helper
# and differs from frames_asm_probe_ok only in the entry's text, so one thing is wrong, or right, in each. Wrong entries the sensor
# MUST fail:
#   tailb_sp      a tail b with the frame pushed          secondbl     a second bl after the pop
#   below_sp      x30 stored below sp                     local_bl     a bl to a label inside the entry
#   path_br       the br register set by a mov on a path  ldadd_ret    the call-site word read with a relaxed ldadd
#   cfi_only_at_bl  rows right at the bl, wrong elsewhere  cas_load    the call-site word read with a relaxed cas
#   x19_unsaved   x30 in x19, the caller's x19 never saved  x29_not_restored  the epilogue reloads x30 only
#   bl_clobbers_br  a br through a register the bl overwrote  slot_overwrite  a spill over x30's save slot
#   slot_revived  a second bl relying on a slot sp passed    fde_short   .cfi_endproc before the bl
#   ret_reg       ret x1                                     remember_no_restore  a path under the wrong row after .cfi_remember_state
# and correct entries it MUST pass:
#   x30_in_x19    x30 kept in x19 (saved) with .cfi_register    x30_in_x20_str  the same, x20 saved with a str
#   clang_fp clang_h clang_k   NDK clang -O2 output           fp_cfa   the CFA on x29     frame32_slot  a 32-byte frame
#   loop          a call in a loop                              passthrough_last   the pass-through br placed last
#   str_x30_only  only x30 saved                                sub_then_stp   sub sp, then stp with no writeback
#   two_exits_remember   two returns under .cfi_remember_state
# and the plugin's three entries (frames_asm_probe_real.cpp): real_naked as the plugin writes them (the sensor MUST fail them),
# real_moved as top-level asm() with CFI and ldar loads (it MUST pass them), real_moved_pbldr with powerboost's plain ldr (#2216;
# it MUST fail that entry).
set(NEVR_ASM_ENTRY_PROBES
  tailb_sp secondbl below_sp local_bl path_br ldadd_ret cfi_only_at_bl x30_in_x19
  cas_load x19_unsaved x29_not_restored bl_clobbers_br slot_overwrite slot_revived fde_short ret_reg remember_no_restore
  x30_in_x20_str clang_fp clang_h clang_k fp_cfa frame32_slot loop passthrough_last str_x30_only sub_then_stp two_exits_remember)
foreach(variant ${NEVR_ASM_ENTRY_PROBES})
  add_executable(frames_asm_probe_${variant}
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_asm_probes/${variant}.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_probe_helper_ok.cpp")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_asm_probes/${variant}.cpp"
                              PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
  target_include_directories(frames_asm_probe_${variant} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel" "${CMAKE_CURRENT_SOURCE_DIR}/../tests" "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  target_link_libraries(frames_asm_probe_${variant} PRIVATE nevr_quest_got_hook)
  target_compile_options(frames_asm_probe_${variant} PRIVATE -Wall -Wextra -Werror)
endforeach()

foreach(variant real_naked real_moved real_moved_pbldr)
  add_executable(frames_asm_probe_${variant}
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_asm_probe_real.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_probe_helper_ok.cpp")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../tests/frames_asm_probe_real.cpp"
                              PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
  if(variant STREQUAL "real_naked")
    target_compile_definitions(frames_asm_probe_real_naked PRIVATE PROBE_REAL_NAKED=1)
  elseif(variant STREQUAL "real_moved_pbldr")
    target_compile_definitions(frames_asm_probe_real_moved_pbldr PRIVATE PROBE_REAL_PLAIN_LDR=1)
  endif()
  target_include_directories(frames_asm_probe_${variant} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel" "${CMAKE_CURRENT_SOURCE_DIR}/../tests" "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  target_link_libraries(frames_asm_probe_${variant} PRIVATE nevr_quest_got_hook)
  target_compile_options(frames_asm_probe_${variant} PRIVATE -Wall -Wextra -Werror)
endforeach()

# The sentinel links the integration, and with it every library package. (Defined here because the
# integration is the first place all of them exist; CMake resolves the target name at generate time.)
target_link_libraries(ovrplatformloader PRIVATE nevr_quest_integration)
