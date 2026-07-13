# libovrplatformloader.so — the breakpad crash-reporter shim that hijacks the
# APK-bundled Oculus Platform loader.

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/breakpad.cmake)

add_library(ovrplatformloader SHARED
  entry.cpp
  sentinel.cpp)

# Emit libovrplatformloader.so (CMake adds the lib prefix + .so suffix).
set_target_properties(ovrplatformloader PROPERTIES OUTPUT_NAME ovrplatformloader)

target_include_directories(ovrplatformloader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/breakpad/src)

target_link_libraries(ovrplatformloader PRIVATE breakpad_client log dl)

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
