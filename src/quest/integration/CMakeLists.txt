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
# reads the same on both platforms. plugin_game_image.cpp and input_seam_hook.cpp reach pinned_targets.h, so they are built
# without exceptions.
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
  "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ext/plugin_load_plan.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_game_image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/input_seam_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_include_directories(nevr_quest_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_include_directories(nevr_quest_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel")
target_link_libraries(nevr_quest_plugin PUBLIC nevr::plugin_sdk nevr_quest_config nevr_quest_got_hook nevr_quest_data nlohmann_json::nlohmann_json)
# Hidden visibility: linked into the sentinel, nothing here may become an export (tests/quest TestExportAllowlist).
target_compile_options(nevr_quest_plugin PRIVATE -Wall -Wextra -Werror -fvisibility=hidden -fvisibility-inlines-hidden)

add_library(nevr_quest_integration STATIC
  ctor_sequence.cpp
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

# The q15t plugin (#1546): libq15t.so and libq15t_early.so, built here and carried by no APK the release or package-dev recipes
# build; `just build-quest-q15t` builds them, and the variant recipe packages them.
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

# The sentinel links the integration, and with it every library package. (Defined here because the
# integration is the first place all of them exist; CMake resolves the target name at generate time.)
target_link_libraries(ovrplatformloader PRIVATE nevr_quest_integration)
