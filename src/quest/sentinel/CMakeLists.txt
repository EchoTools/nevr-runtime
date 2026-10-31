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
                         "${NEVR_EXTRA_ROWS_JSON_INCLUDE}" "${CMAKE_CURRENT_BINARY_DIR}/extra_rows_check")
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
  quit_hook.cpp
  vk_free_guard.cpp
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
  activation.cpp
  credits_text.cpp
  "${NEVR_QUEST_CREDITS_BLOCK_CPP}")

# Emit libovrplatformloader.so (CMake adds the lib prefix + .so suffix).
set_target_properties(ovrplatformloader PROPERTIES OUTPUT_NAME ovrplatformloader)

target_include_directories(ovrplatformloader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/breakpad/src)

target_link_libraries(ovrplatformloader PRIVATE nevr::plugin_sdk nevr_quest_got_hook nevr_quest_config nlohmann_json::nlohmann_json nevr_quest_prompt_board nevr_quest_data_wiring nevr_quest_invite_join nevr_quest_data_net nevr_quest_data nevr_quest_symcache breakpad_client log dl)

# The hook path (entry.cpp and login_prompt_hook.cpp: the thunk entries and the handlers they
# call; the prompt board the login-prompt handler reads is built the same way, see
# src/quest/CMakeLists.txt) is compiled without exception support so its frames carry no
# personality or LSDA; see the contract in callback_thunk.h. tests/quest
# TestHookFramesCarryNoPersonality pins it.
set_source_files_properties(entry.cpp login_prompt_hook.cpp ../invite/frame_dispatch.cpp main_level.cpp login_title_hook.cpp obb_skip_hook.cpp path_rewrite_hook.cpp process_end_hook.cpp crash_first_hook.cpp permission_request.cpp permission_wait_hook.cpp vk_ext_window.cpp os_build_gate_hook.cpp resource_request_hook.cpp render_config_hook.cpp menu_cards_hook.cpp ../invite/wizard_metric_hook.cpp texture_quality_hook.cpp texture_menu_hook.cpp ui_texture_mips.cpp quit_hook.cpp vk_free_guard.cpp pns_provider_hook.cpp toast_presenter.cpp credits_text.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")

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

# Only nevr_sentinel_marker and JNI_OnLoad are exported (both carry explicit default
# visibility); everything else, including the hook API and the test seams, is hidden.
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
