# libovrplatformloader.so — the breakpad crash-reporter shim that hijacks the
# APK-bundled Oculus Platform loader.

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/breakpad.cmake)

# The GOT backend and its log live in ONE static library. Every Quest target that
# hooks links this target; compiling the backend source again elsewhere would give that
# copy its own write lock and slot registry (the justfile's test-quest-hooks fails
# if a second CMake list names the file).
add_library(nevr_quest_got_hook STATIC
  got_hook.cpp
  hook_log.cpp)
target_include_directories(nevr_quest_got_hook PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_options(nevr_quest_got_hook PRIVATE
  -Wall -Wextra -Werror -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(nevr_quest_got_hook PUBLIC log dl)

add_library(ovrplatformloader SHARED
  entry.cpp
  sentinel.cpp
  sentinel_log.cpp
  activation.cpp)

# Emit libovrplatformloader.so (CMake adds the lib prefix + .so suffix).
set_target_properties(ovrplatformloader PROPERTIES OUTPUT_NAME ovrplatformloader)

target_include_directories(ovrplatformloader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/breakpad/src)

target_link_libraries(ovrplatformloader PRIVATE nevr_quest_got_hook nevr_quest_config breakpad_client log dl)

# The hook path (entry.cpp: the thunk entry and the handler it calls) is compiled
# without exception support so its frames carry no personality or LSDA; see the
# contract in callback_thunk.h. tests/quest TestHookFramesCarryNoPersonality pins it.
set_source_files_properties(entry.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")

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

# The config, activation and log sources build warning-clean (the host test builds them the same way).
set_source_files_properties(activation.cpp sentinel_log.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
