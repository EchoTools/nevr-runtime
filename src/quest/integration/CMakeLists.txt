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
# reads the same on both platforms. plugin_game_image.cpp reaches pinned_targets.h, so it is built without exceptions.
add_library(nevr_quest_plugin STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_plan.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_loader.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_record_log.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_platform.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_runtime.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_game_image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ext/plugin_load_plan.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../plugin/plugin_game_image.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_include_directories(nevr_quest_plugin PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_include_directories(nevr_quest_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel")
target_link_libraries(nevr_quest_plugin PUBLIC nevr_quest_config nevr_quest_got_hook nevr_quest_data nlohmann_json::nlohmann_json)
# Hidden visibility: linked into the sentinel, nothing here may become an export (tests/quest TestExportAllowlist).
target_compile_options(nevr_quest_plugin PRIVATE -Wall -Wextra -Werror -fvisibility=hidden -fvisibility-inlines-hidden)

add_library(nevr_quest_integration STATIC
  ctor_sequence.cpp
  post_load.cpp
  identity_source.cpp
  stage_log.cpp
  bridge_uri.cpp
  production_steps.cpp
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
target_link_libraries(integration_hooks_test PRIVATE nevr_quest_prompt_board)
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

# A no-op plugin for a headset run of the loader (docs/adr/0019, slice 1): the host test's fixture, named `nevr_noop` and
# declaring the libr15 build the sentinel is pinned to. It is not part of the sentinel or of any APK the recipes build; a
# development APK gets it from `just android-repack-apk ... plugin=<this file>`.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel/pinned_targets.h" nevr_pinned_build_id_line
     REGEX "kLibR15BuildId = \"[0-9a-f]+\"")
string(REGEX MATCH "\"([0-9a-f]+)\"" nevr_pinned_build_id_match "${nevr_pinned_build_id_line}")
set(nevr_pinned_build_id "${CMAKE_MATCH_1}")
if(NOT nevr_pinned_build_id)
  message(FATAL_ERROR "src/quest/sentinel/pinned_targets.h has no kLibR15BuildId literal")
endif()
add_library(nevr_noop_plugin SHARED "${CMAKE_CURRENT_SOURCE_DIR}/../tests/plugin_fixture.cpp")
set_target_properties(nevr_noop_plugin PROPERTIES OUTPUT_NAME nevr_noop)
target_compile_definitions(nevr_noop_plugin PRIVATE FX_NAME="nevr_noop" FX_IDS="${nevr_pinned_build_id}" FX_NO_CTOR)
target_include_directories(nevr_noop_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_options(nevr_noop_plugin PRIVATE -Wall -Wextra -Werror)
target_link_options(nevr_noop_plugin PRIVATE -Wl,--build-id=sha1)

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
