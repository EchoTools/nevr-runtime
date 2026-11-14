# libovrplatformloader.so — the breakpad crash-reporter shim that hijacks the
# APK-bundled Oculus Platform loader.

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/breakpad.cmake)

# The GOT backend, its log and the counter reporter live in ONE static library. Every Quest
# target that hooks links this target; compiling the backend source again elsewhere would give
# that copy its own write lock and slot registry. tests/quest TestBackendCompiledOnce counts
# the compile rules in the configured build graph.
#
# Built without exceptions, like the host test build (just test-quest-hooks), so the host tests
# exercise the same code generation as the artifact and the backend's frames carry no
# personality.
add_library(nevr_quest_got_hook STATIC
  got_hook.cpp
  arm64_reloc.cpp
  libr15_attach.cpp
  libr15_identity.cpp
  ../patch/libr15_patch_identity.cpp
  hook_log.cpp
  hook_report.cpp)
target_include_directories(nevr_quest_got_hook PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_options(nevr_quest_got_hook PRIVATE
  -Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden)
# libr15_attach.cpp derives the patched build ID from the compiled-in table (ADR 0021 section 7): the identity source and
# the SHA-256 in nevr_quest_data, which this static library pulls in only for that object.
target_link_libraries(nevr_quest_got_hook PUBLIC nevr_quest_data log dl)

# The extra rows of a variant build (NEVR_LIBR15_EXTRA_ROWS, src/quest/CMakeLists.txt; docs/adr/0021 section 11). The public
# build adds nothing here. A variant build checks the file with the patcher's parser at configure time, embeds its text and digest,
# and links the parser so libr15_identity.cpp can read the rows back and compute the same patched build ID as its libr15.
if(NEVR_LIBR15_EXTRA_ROWS)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/libr15_extra_rows.cmake)
  get_filename_component(NEVR_EXTRA_ROWS_FILE "${NEVR_LIBR15_EXTRA_ROWS}" ABSOLUTE BASE_DIR "${NEVR_REPO_ROOT}")
  get_target_property(NEVR_EXTRA_ROWS_JSON_INCLUDE nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
  list(GET NEVR_EXTRA_ROWS_JSON_INCLUDE 0 NEVR_EXTRA_ROWS_JSON_INCLUDE)
  set(NEVR_EXTRA_ROWS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/libr15_extra_rows_data.cpp")
  nevr_libr15_extra_rows("${NEVR_EXTRA_ROWS_FILE}" "${NEVR_EXTRA_ROWS_SOURCE}" "${NEVR_REPO_ROOT}/src"
                         "${NEVR_EXTRA_ROWS_JSON_INCLUDE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NEVR_EXTRA_ROWS_FILE}")
  target_sources(nevr_quest_got_hook PRIVATE ../patch/libr15_extra_rows.cpp "${NEVR_EXTRA_ROWS_SOURCE}")
  target_compile_definitions(nevr_quest_got_hook PRIVATE NEVR_LIBR15_EXTRA_ROWS=1)
  target_link_libraries(nevr_quest_got_hook PRIVATE nlohmann_json::nlohmann_json)
endif()

# The first-run data step (#807). Its std::function lambdas instantiate libc++ templates whose type info is exported
# from an object file of the shared library; in an archive, --exclude-libs (below) keeps it out of the export set
# (tests/quest TestExportAllowlist). It calls into the hook sources that are the shared library's own objects.
add_library(nevr_quest_data_wiring STATIC data_wiring.cpp)
target_include_directories(nevr_quest_data_wiring PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_options(nevr_quest_data_wiring PRIVATE
  -Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(nevr_quest_data_wiring PUBLIC nevr_quest_got_hook nevr_quest_data nevr_quest_data_net)

# #970: the project section of the in-game CREDITS page is rendered from config/credits.json by the same tool the
# PC build runs (src/runtime/CMakeLists.txt), so both platforms show the same bytes. The generated source is
# compiled into ovrplatformloader; credits_text.cpp points the page at it (sentinel/credits_text.h).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(NEVR_QUEST_CREDITS_BLOCK_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/credits_block.cpp")
add_custom_command(
  OUTPUT "${NEVR_QUEST_CREDITS_BLOCK_CPP}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/render_credits.py"
          --ingame-source "${NEVR_QUEST_CREDITS_BLOCK_CPP}"
          --json "${CMAKE_CURRENT_SOURCE_DIR}/../../../config/credits.json"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../../config/credits.json"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/render_credits.py"
  COMMENT "Rendering the in-game credits block from config/credits.json"
  VERBATIM)

add_library(ovrplatformloader SHARED
  entry.cpp
  clock_levels_hook.cpp
  login_prompt_hook.cpp
  ../invite/frame_dispatch.cpp
  main_level.cpp
  login_title_hook.cpp
  obb_skip_hook.cpp
  path_rewrite_hook.cpp
  process_end_hook.cpp
  crash_first_hook.cpp
  permission_request.cpp
  permission_wait_hook.cpp
  vk_ext_window.cpp
  texture_quality_hook.cpp
  texture_menu_hook.cpp
  texture_menu.cpp
  render_config_hook.cpp
  ui_texture_mips.cpp
  irradiance_default_hook.cpp
  irradiance_default_install.cpp
  quit_hook.cpp
  hand_pose_log_hook.cpp
  vk_free_guard.cpp
  vk_alloc_check.cpp
  pns_provider_hook.cpp
  os_build_gate_hook.cpp
  resource_request_hook.cpp
  menu_cards_hook.cpp
  ../invite/wizard_metric_hook.cpp
  toast_presenter.cpp
  android_glue_layout_check.cpp
  sentinel.cpp
  sentinel_log.cpp
  crash_report_quest.cpp
  elf_build_id.cpp
  ../../core/crash_report.cpp
  ../../core/resource_shadow.cpp
  ../../core/cargo_resolver.cpp
  ../../core/cargo_limits.cpp
  ../../core/cargo_validate.cpp
  ../../core/cargo_manifest.cpp
  ../../core/cargo_plan.cpp
  ../../core/cargo_store.cpp
  ../cargo/posix_env.cpp
  activation.cpp
  credits_text.cpp
  ec_page.cpp
  "${NEVR_QUEST_CREDITS_BLOCK_CPP}")

# The #1737 measurement probe, only in a build configured with NEVR_QUEST_PERF_PROBE=ON (src/quest/CMakeLists.txt).
if(NEVR_QUEST_PERF_PROBE)
  target_sources(ovrplatformloader PRIVATE perf_probe_hook.cpp)
  # set_property, so the wiring tests keep finding the sentinel's one set_source_files_properties list of -fno-exceptions sources
  set_property(SOURCE perf_probe_hook.cpp PROPERTY COMPILE_OPTIONS "-fno-exceptions")
endif()

# Emit libovrplatformloader.so (CMake adds the lib prefix + .so suffix).
set_target_properties(ovrplatformloader PROPERTIES OUTPUT_NAME ovrplatformloader)

target_include_directories(ovrplatformloader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/breakpad/src)

target_link_libraries(ovrplatformloader PRIVATE zstd::libzstd nevr::plugin_sdk nevr_quest_got_hook nevr_quest_config nlohmann_json::nlohmann_json nevr_quest_prompt_board nevr_quest_data_wiring nevr_quest_invite_join nevr_quest_cargo nevr_quest_combat nevr_quest_data_net nevr_quest_data nevr_quest_atomic_write nevr_quest_symcache breakpad_client log dl)

# The hook path (entry.cpp and login_prompt_hook.cpp: the thunk entries and the handlers they
# call; the prompt board the login-prompt handler reads is built the same way, see
# src/quest/CMakeLists.txt) is compiled without exception support so its frames carry no
# personality or LSDA; see the contract in callback_thunk.h. tests/quest
# TestHookFramesCarryNoPersonality pins it.
# ec_page.cpp is on the hook path (the EC client's handlers reach the page through it): built without exceptions, as the combat code is.
set_source_files_properties(entry.cpp ec_page.cpp login_prompt_hook.cpp ../invite/frame_dispatch.cpp main_level.cpp login_title_hook.cpp obb_skip_hook.cpp path_rewrite_hook.cpp process_end_hook.cpp crash_first_hook.cpp permission_request.cpp permission_wait_hook.cpp vk_ext_window.cpp os_build_gate_hook.cpp resource_request_hook.cpp ../../core/resource_shadow.cpp ../../core/cargo_resolver.cpp ../../core/cargo_limits.cpp ../../core/cargo_validate.cpp ../../core/cargo_manifest.cpp ../../core/cargo_plan.cpp ../../core/cargo_store.cpp ../../core/cargo_run.cpp ../cargo/posix_env.cpp ../cargo/cargo_service.cpp ../cargo/cargo_start.cpp render_config_hook.cpp menu_cards_hook.cpp ../invite/wizard_metric_hook.cpp texture_quality_hook.cpp texture_menu_hook.cpp ui_texture_mips.cpp irradiance_default_hook.cpp irradiance_default_install.cpp quit_hook.cpp hand_pose_log_hook.cpp vk_free_guard.cpp vk_alloc_check.cpp pns_provider_hook.cpp toast_presenter.cpp credits_text.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")

# The Quest join on a resolved invite code (#966, S4). Its std::function<bool()> and std::function<std::int64_t()>
# come through the shared invite_join.h; libc++ marks those templates' type info default-visibility, so an object
# linked straight into libovrplatformloader.so exports it (tests/quest TestExportAllowlist). In an archive,
# --exclude-libs (below) keeps it out of the export set, the same as nevr_quest_data_wiring. It is on the hook path
# (entry.cpp calls RegisterAnswerApplier, reached from HookedNetGameUpdate), so it is built without exceptions, and with hidden
# default visibility and the warnings-as-errors flags its siblings nevr_quest_data_wiring and nevr_quest_invite use.
add_library(nevr_quest_invite_join STATIC ../invite/join_game_quest.cpp ../invite/ui_text_quest.cpp)
target_include_directories(nevr_quest_invite_join PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_BINARY_DIR})
target_compile_options(nevr_quest_invite_join PRIVATE
  -Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(nevr_quest_invite_join PUBLIC nevr_quest_got_hook nlohmann_json::nlohmann_json)

# The cargo fetch and mount (#2135). cargo_run.cpp, cargo_service.cpp and cargo_start.cpp take std::function<bool()>,
# std::function<bool(long, const void*, size_t)> and std::function<bool(const std::string&)> arguments; libc++ marks those
# templates' type info default-visibility, so an object linked straight into libovrplatformloader.so exports it
# (tests/quest TestExportAllowlist). In an archive, --exclude-libs (above) keeps it out of the export set, the same as
# nevr_quest_data_wiring and nevr_quest_invite_join. The sources stay in the -fno-exceptions list above (a directory-scoped
# source property, so it applies here), and the flags match their siblings.
add_library(nevr_quest_cargo STATIC ../../core/cargo_run.cpp ../../core/cargo_prefetch.cpp ../cargo/cargo_service.cpp ../cargo/cargo_start.cpp)
target_include_directories(nevr_quest_cargo PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_BINARY_DIR})
target_compile_options(nevr_quest_cargo PRIVATE
  -Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(nevr_quest_cargo PUBLIC nevr_quest_got_hook nlohmann_json::nlohmann_json zstd::libzstd)  # the manifest is one zstd frame (core/cargo_manifest.cpp)

# The CSymbol64 name table the UI texture hook reads to tell a UI texture from a world texture: the merged PC and Quest
# symbol sets, sorted by hash (src/runtime/log/symcache_data.cpp, generated by tools/symcache). It is data only; its
# own object, so the hook's frames stay in ui_texture_mips.cpp.
add_library(nevr_quest_symcache STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/log/symcache_data.cpp)
target_include_directories(nevr_quest_symcache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# android_glue_layout_check.cpp asserts the glue offsets against the NDK's own native_app_glue header.
if(DEFINED ANDROID_NDK)
  set_source_files_properties(android_glue_layout_check.cpp PROPERTIES
    COMPILE_FLAGS "-I${ANDROID_NDK}/sources/android/native_app_glue")
elseif(DEFINED CMAKE_ANDROID_NDK)
  set_source_files_properties(android_glue_layout_check.cpp PROPERTIES
    COMPILE_FLAGS "-I${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
else()
  message(FATAL_ERROR "the NDK directory is unknown: android_glue_layout_check.cpp needs its native_app_glue header")
endif()

# Only nevr_sentinel_marker, JNI_OnLoad and re33_ec_page (the EC script libraries' page, ec_page.h) are exported (each carries
# explicit default visibility); everything else, including the hook API and the test seams, is hidden.
# tests/quest TestExportAllowlist pins the list.
target_compile_options(ovrplatformloader PRIVATE
  -Wall -Wextra -Werror -fvisibility=hidden -fvisibility-inlines-hidden)

# Keep breakpad's internal symbols from leaking out of our public export set.
target_link_options(ovrplatformloader PRIVATE -Wl,--exclude-libs,ALL)

# Forward to the renamed original: add a DT_NEEDED on libovrplatformloader_orig.so
# so Bionic pulls the real loader into the process (its exports then resolve to
# consumers via local-group lookup). patchelf is deterministic and needs no stub
# lib at link time (BAC-3).
find_program(PATCHELF patchelf REQUIRED)
add_custom_command(TARGET ovrplatformloader POST_BUILD
  COMMAND ${PATCHELF} --add-needed libovrplatformloader_orig.so
          $<TARGET_FILE:ovrplatformloader>
  COMMENT "patchelf: add DT_NEEDED libovrplatformloader_orig.so (forward-to-original)"
  VERBATIM)

# ── Quest runtime library (libnevr-runtime.so) ──────────────────────────────────────────────────────
# The runtime is what RunSentinelConstructor runs (integration/production_steps.cpp): the sentinel's sources and libraries,
# built again as a second shared object that exports one entry point, NEVR_GetRuntimeApi (core/quest_runtime_abi.h,
# runtime/runtime_entry.cpp). The sentinel is unchanged: it still runs its own copy in process, and nothing in this tree
# loads libnevr-runtime.so.
#
# It runs the sentinel's whole constructor sequence (RunSentinelConstructor, in nevr_quest_integration): sentinel::Arm, the
# crash-first and dlopen hooks, the sentinel log writer, the hook registry (sentinel::RegistrySnapshot) and the process-end
# hook. A process must not map it while its sentinel is armed (sentinel::Arm has run). Who owns that sequence under the
# host-as-entry design is held.
#
# The sources, libraries, include directories and compile options are read from the sentinel's target above, so the copy
# cannot drift from it. nevr_quest_integration is the one library the sentinel gets from elsewhere
# (integration/CMakeLists.txt attaches it to ovrplatformloader after this file runs), so it is named here.
#
# The copy has no load-time entry: NEVR_QUEST_RUNTIME_LIBRARY, set below, leaves nevr_sentinel_ctor, the marker and JNI_OnLoad
# out of entry.cpp. tools/check_quest_static_init.sh --plugin (justfile, _check-quest-runtime-library) allows no initializer
# but the two compiler-rt entries. -Wl,-z,text makes a text relocation a link error; --exclude-libs,ALL keeps the libraries'
# symbols out of the export table, as it does for the sentinel.
include("${NEVR_REPO_ROOT}/cmake/nevr_build_identity.cmake")
# PROJECT_VERSION, GIT_COMMIT_HASH, GIT_COMMIT_HASH_FULL and GIT_DESCRIBE, in this directory's scope: the identity literal of
# core/build_identity.cpp is compiled from them (nevr_build_identity_define below).
set_project_version_from_git()

get_target_property(NEVR_QUEST_RUNTIME_SOURCES ovrplatformloader SOURCES)
get_target_property(NEVR_QUEST_RUNTIME_LIBRARIES ovrplatformloader LINK_LIBRARIES)
get_target_property(NEVR_QUEST_RUNTIME_INCLUDES ovrplatformloader INCLUDE_DIRECTORIES)
get_target_property(NEVR_QUEST_RUNTIME_OPTIONS ovrplatformloader COMPILE_OPTIONS)

add_library(nevr_quest_runtime SHARED
  ${NEVR_QUEST_RUNTIME_SOURCES}
  ../runtime/runtime_entry.cpp
  "${NEVR_REPO_ROOT}/src/core/build_identity.cpp")
set_target_properties(nevr_quest_runtime PROPERTIES OUTPUT_NAME nevr-runtime)
target_compile_definitions(nevr_quest_runtime PRIVATE NEVR_QUEST_RUNTIME_LIBRARY=1)
target_include_directories(nevr_quest_runtime PRIVATE ${NEVR_QUEST_RUNTIME_INCLUDES})
target_compile_options(nevr_quest_runtime PRIVATE ${NEVR_QUEST_RUNTIME_OPTIONS})
target_link_libraries(nevr_quest_runtime PRIVATE ${NEVR_QUEST_RUNTIME_LIBRARIES} nevr_quest_integration)
# -Wl,-z,defs makes an undefined symbol a link error: every name the copy uses resolves against a library the link names.
target_link_options(nevr_quest_runtime PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,text -Wl,-z,defs)

# The runtime's own NEVR-BUILD literal (core/build_identity.h): the version and the full commit, the same identity the PC
# runtime embeds. A binary contains exactly one; the runtime's copy is the only one in this library.
nevr_build_identity_define(TARGET nevr_quest_runtime
  READS VERSION COMMIT_HASH COMMIT_FULL DESCRIBE
  SOURCES "${NEVR_REPO_ROOT}/src/core/build_identity.cpp")
