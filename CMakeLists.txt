cmake_minimum_required(VERSION 4.0.0)

# Suppress vcpkg root mismatch warning
unset(ENV{VCPKG_ROOT})

# Suppress vcpkg usage text spam
set(X_VCPKG_APPLOCAL_DEPS_INSTALL OFF CACHE BOOL "" FORCE)
set(VCPKG_INSTALL_OPTIONS "--no-print-usage" CACHE STRING "" FORCE)
set(Z_VCPKG_BUILTIN_POWERSHELL_PATH "" CACHE STRING "" FORCE)

# Suppress vcpkg install output
function(message)
  list(GET ARGV 0 MessageType)
  if(MessageType STREQUAL "STATUS")
    list(GET ARGV 1 MessageText)
    # Suppress vcpkg installation messages
    if(MessageText MATCHES "^Running vcpkg install" OR 
       MessageText MATCHES "^The following packages" OR
       MessageText MATCHES "^\\*" OR
       MessageText MATCHES "^  " OR
       MessageText MATCHES "^All requested installations completed")
      return()
    endif()
  endif()
  _message(${ARGV})
endfunction()

# Project Name
project(NEVRServer LANGUAGES C CXX)

# import the version from the git-version.cmake file
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/set_project_version_from_git.cmake")

# Set PROJECT_VERSION and GIT_COMMIT_HASH based on the current git state.
set_project_version_from_git()

# Define the version and git hash as compile definitions
add_compile_definitions(
  PROJECT_VERSION_MAJOR="${PROJECT_VERSION_MAJOR}"
  PROJECT_VERSION_MINOR="${PROJECT_VERSION_MINOR}"
  PROJECT_VERSION_PATCH="${PROJECT_VERSION_PATCH}"
  PROJECT_VERSION_TWEAK="${PROJECT_VERSION_TWEAK}"
  PROJECT_VERSION="${PROJECT_VERSION}"
  GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
  GIT_DESCRIBE="${GIT_DESCRIBE}")

# Print out the version that was set by set_project_version_from_git()
message(STATUS "Project version: ${PROJECT_VERSION} (${GIT_COMMIT_HASH})")

# Set the project output name
set(PROJECT_OUTPUT_NAME "${CMAKE_PROJECT_NAME}")

# Set the project description
set(PROJECT_DESCRIPTION "NEVR Game Server")

# Set the project vendor
set(PROJECT_VENDOR "EchoTools")

# Set the project organization
set(PROJECT_ORGANIZATION "EchoTools")

# Set the project URL
set(PROJECT_URL "https://github.com/echotools/nevr-runtime")

# set(CMAKE_BUILD_TYPE "Release")

# Check if the CMAKE_BUILD_TYPE is set, if not, default to Debug
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

message(STATUS "Build type set to: ${CMAKE_BUILD_TYPE}")

# Disable verbose makefile output
set(CMAKE_VERBOSE_MAKEFILE OFF)

# Set the CMake generator to Ninja
set(CMAKE_GENERATOR "Ninja")

# Set the CMake executable linker flags (MSVC only)
if(MSVC)
  set(CMAKE_EXE_LINKER_FLAGS "/SUBSYSTEM:WINDOWS")
endif()

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC-specific settings
if(MSVC)
  # Add LTO for Release
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  
  # Disable UAC
  add_link_options(/MANIFESTUAC:NO)
  
  # Enforce Static Runtime Libraries
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Set the output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Enable CMake to generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVC-specific compile options
if(MSVC)
  add_compile_options(
    "/Zi" # Enable debugging information
    "/W3" # Warning level 3
    "/sdl" # Enable additional security checks
    "/permissive-" # Enforce standard conformance
    "/MT" # Use static runtime library
    "/EHsc" # Enable C++ exceptions
    "$<$<CONFIG:Debug>:/D_DEBUG>" # Debug: Define _DEBUG
    "$<$<CONFIG:Release>:/DNDEBUG>" # Release: Define NDEBUG
    "$<$<CONFIG:Release>:/Od>" # Debug: Disable optimizations

    # "$<$<CONFIG:Release>:/O2>" # Release: Enable optimizations
  )
endif()

# MinGW-specific compile options
if(MINGW)
  add_compile_options(
    -Wall
    -Wextra
    # Disable specific warnings that are unavoidable in this codebase
    -Wno-unknown-pragmas           # Windows-specific pragmas like #pragma comment
    -Wno-unused-parameter          # DLL export functions have required but unused params
    -Wno-cast-function-type        # Windows API requires function pointer casts
    -Wno-sign-compare              # Mixing signed/unsigned in legacy loops
    -Wno-maybe-uninitialized       # False positives on struct initialization
    -Wno-pragma-once-outside-header # .cpp files misused as headers in legacy code
    "$<$<CONFIG:Debug>:-g>"
    "$<$<CONFIG:Debug>:-O0>"
    "$<$<CONFIG:Release>:-O3>"
    "$<$<CONFIG:Release>:-DNDEBUG>"
  )
  # C++-only warning suppressions
  add_compile_options(
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-conversion-null>"        # NULL to UINT64 conversions in legacy code
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-delete-non-virtual-dtor>" # Game engine interface has no virtual dtor
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder>"                # Member initialization order in legacy code
  )
endif()

# MSVC-specific link options
if(MSVC)
  add_link_options(
    "$<$<CONFIG:Debug>:/DEBUG>" # Debug: Generate debug information
    "$<$<CONFIG:Debug>:/SUBSYSTEM:WINDOWS>" # Debug: Set subsystem to Windows
    "$<$<CONFIG:Release>:/DEBUG>" # Debug: Generate debug information
    "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>" # Release: Set subsystem to Windows
    "$<$<CONFIG:Release>:/OPT:REF>" # Release: Enable reference optimization
    "$<$<CONFIG:Release>:/OPT:ICF>" # Release: Enable identical COMDAT folding
  )
endif()

# MinGW-specific link options
if(MINGW)
  add_link_options(
    -mwindows
    -static
    -static-libgcc
    -static-libstdc++
  )
endif()

# Include directories
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_BINARY_DIR}
  "${CMAKE_SOURCE_DIR}/src")

# Define preprocessor macros
add_compile_definitions("$<$<CONFIG:Debug>:_DEBUG;_WINDOWS;_USRDLL>"
  "$<$<CONFIG:Release>:NDEBUG;_WINDOWS;_USRDLL>")

add_subdirectory(extern)

find_package(Protobuf REQUIRED)

add_subdirectory(src/common)
add_subdirectory(src/protobufnevr)
add_subdirectory(src/gamepatches)
add_subdirectory(src/legacy/gamepatches)
add_subdirectory(src/gameserver)
add_subdirectory(src/legacy/gameserver)
add_subdirectory(src/server)
# telemetryagent moved to extras/ — needs protobuf-lite refactor before reintegration

add_subdirectory(plugins)

# Include a local cmake script if it exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local.cmake")
  # include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/local.cmake")
endif()

# =============================================================================
# Distribution Package Target
# =============================================================================
# Creates distribution archives (tar.zst and zip) containing DLLs and scripts
# Usage: cmake --build <build_dir> --target dist
# =============================================================================

set(DIST_NAME "nevr-runtime-v${PROJECT_VERSION}")
set(DIST_DIR "${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}")

# List of DLL artifacts to include
set(DIST_DLLS
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gamepatches.dll"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gamepatcheslegacy.dll"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserver.dll"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserverlegacy.dll"
)

# List of debug symbol files to include
set(DIST_DBG_FILES
  "${CMAKE_BINARY_DIR}/gamepatches.dll.dbg"
  "${CMAKE_BINARY_DIR}/gamepatcheslegacy.dll.dbg"
  "${CMAKE_BINARY_DIR}/gameserver.dll.dbg"
  "${CMAKE_BINARY_DIR}/gameserverlegacy.dll.dbg"
)

# Custom target to prepare distribution directory
add_custom_target(dist-prepare
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_SOURCE_DIR}/dist"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
  # Copy and rename gamepatches.dll -> dbgcore.dll (if exists)
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gamepatches.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gamepatches.dll" "${DIST_DIR}/dbgcore.dll" || true
  # Copy and rename gameserver.dll -> pnsradgameserver.dll (if exists)
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserver.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserver.dll" "${DIST_DIR}/pnsradgameserver.dll" || true
  # Copy remaining DLLs with original names (if exist)
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gamepatcheslegacy.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gamepatcheslegacy.dll" "${DIST_DIR}/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserverlegacy.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserverlegacy.dll" "${DIST_DIR}/" || true
  # Copy executables (launchers and loaders, if exist)
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/echovr_launcher.exe" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/echovr_launcher.exe" "${DIST_DIR}/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/echovr_patched.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/echovr_patched.dll" "${DIST_DIR}/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/echovr_server.exe" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/echovr_server.exe" "${DIST_DIR}/" || true
  #COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DllLoader.exe" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DllLoader.exe" "${DIST_DIR}/" || true
   COMMAND test -f "${CMAKE_SOURCE_DIR}/README.md" && ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/README.md" "${DIST_DIR}/" || true
  # Copy plugin DLLs into plugins/ subdirectory
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/plugins"
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/log_filter.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/log_filter.dll" "${DIST_DIR}/plugins/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/server_timing.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/server_timing.dll" "${DIST_DIR}/plugins/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/broadcaster_bridge.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/broadcaster_bridge.dll" "${DIST_DIR}/plugins/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/audio_intercom.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/audio_intercom.dll" "${DIST_DIR}/plugins/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/game_rules_override.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/game_rules_override.dll" "${DIST_DIR}/plugins/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/session_unlocker.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/session_unlocker.dll" "${DIST_DIR}/plugins/" || true
  COMMAND test -f "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/social_bridge.dll" && ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/social_bridge.dll" "${DIST_DIR}/plugins/" || true
  VERBATIM
)

# =============================================================================
# Code Signing
# =============================================================================
# Signs DLLs/EXEs in the distribution directory using osslsigncode (Linux) or
# signtool (Windows). Credentials are configured via environment variables:
#   CODESIGN_PFX / CODESIGN_CERT+CODESIGN_KEY / CODESIGN_THUMBPRINT
# Skips gracefully when no credentials are available (default for local builds).
# Set CODESIGN_SKIP=1 to skip explicitly, or provide credentials to enable.
# =============================================================================

find_program(OSSLSIGNCODE osslsigncode)
set(CODESIGN_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/codesign/sign.sh")

# Collect signable artifacts in dist dir (DLLs and EXEs)
set(DIST_SIGN_GLOB "${DIST_DIR}/*.dll" "${DIST_DIR}/*.exe")

if(OSSLSIGNCODE)
  add_custom_target(dist-sign
    COMMAND bash -c "\"${CODESIGN_SCRIPT}\" \"${DIST_DIR}/\"*.dll \"${DIST_DIR}/\"*.exe \"${DIST_DIR}/plugins/\"*.dll"
    DEPENDS dist-prepare
    COMMENT "Code signing distribution artifacts"
  )
else()
  add_custom_target(dist-sign
    COMMAND ${CMAKE_COMMAND} -E echo "WARNING: osslsigncode not found, skipping code signing"
    DEPENDS dist-prepare
    VERBATIM
  )
endif()

# Create tar.zst archive
add_custom_target(dist-tar
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_SOURCE_DIR}/dist"
    tar --zstd -cf "${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}.tar.zst" "${DIST_NAME}"
  DEPENDS dist-sign
  VERBATIM
)

# Create zip archive
add_custom_target(dist-zip
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_SOURCE_DIR}/dist"
    ${CMAKE_COMMAND} -E tar cf "${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}.zip" --format=zip "${DIST_NAME}"
  DEPENDS dist-sign
  VERBATIM
)

# Main distribution target that creates both archives
add_custom_target(dist
  DEPENDS dist-tar dist-zip
)

# =============================================================================
# Lite Distribution Package Target (stripped, no debug symbols)
# =============================================================================
# Creates smaller distribution archives without debug symbols
# Usage: cmake --build <build_dir> --target dist-lite
# =============================================================================

set(DIST_LITE_NAME "nevr-runtime-v${PROJECT_VERSION}-lite")
set(DIST_LITE_DIR "${CMAKE_SOURCE_DIR}/dist/${DIST_LITE_NAME}")

# Find strip tool (use llvm-strip or strip)
find_program(STRIP_TOOL NAMES llvm-strip strip)

# Custom target to prepare lite distribution directory with stripped DLLs
add_custom_target(dist-lite-prepare
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_LITE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_LITE_DIR}"
  # Copy and rename gamepatches.dll -> dbgcore.dll, then strip (if exists)
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gamepatches.dll" "${DIST_LITE_DIR}/dbgcore.dll" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/dbgcore.dll" || ${CMAKE_COMMAND} -E true
  # Copy and rename gameserver.dll -> pnsradgameserver.dll, then strip (if exists)
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserver.dll" "${DIST_LITE_DIR}/pnsradgameserver.dll" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/pnsradgameserver.dll" || ${CMAKE_COMMAND} -E true
  # Copy remaining DLLs with original names and strip (if exist)
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gamepatcheslegacy.dll" "${DIST_LITE_DIR}/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/gamepatcheslegacy.dll" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserverlegacy.dll" "${DIST_LITE_DIR}/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/gameserverlegacy.dll" || ${CMAKE_COMMAND} -E true
  # Copy executables (launchers and loaders, if exist)
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/echovr_launcher.exe" "${DIST_LITE_DIR}/" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/echovr_server.exe" "${DIST_LITE_DIR}/" || ${CMAKE_COMMAND} -E true
  #COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DllLoader.exe" "${DIST_LITE_DIR}/" || ${CMAKE_COMMAND} -E true
   COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/README.md" "${DIST_LITE_DIR}/" || ${CMAKE_COMMAND} -E true
  # Copy and strip plugin DLLs into plugins/ subdirectory
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_LITE_DIR}/plugins"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/log_filter.dll" "${DIST_LITE_DIR}/plugins/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/plugins/log_filter.dll" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/server_timing.dll" "${DIST_LITE_DIR}/plugins/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/plugins/server_timing.dll" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/broadcaster_bridge.dll" "${DIST_LITE_DIR}/plugins/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/plugins/broadcaster_bridge.dll" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/audio_intercom.dll" "${DIST_LITE_DIR}/plugins/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/plugins/audio_intercom.dll" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/game_rules_override.dll" "${DIST_LITE_DIR}/plugins/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/plugins/game_rules_override.dll" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/session_unlocker.dll" "${DIST_LITE_DIR}/plugins/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/plugins/session_unlocker.dll" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/social_bridge.dll" "${DIST_LITE_DIR}/plugins/" && ${STRIP_TOOL} --strip-all "${DIST_LITE_DIR}/plugins/social_bridge.dll" || ${CMAKE_COMMAND} -E true
  COMMENT "Preparing lite distribution directory (stripped): ${DIST_LITE_DIR}"
  VERBATIM
)

# Sign lite distribution artifacts
if(OSSLSIGNCODE)
  add_custom_target(dist-lite-sign
    COMMAND bash -c "\"${CODESIGN_SCRIPT}\" \"${DIST_LITE_DIR}/\"*.dll \"${DIST_LITE_DIR}/\"*.exe \"${DIST_LITE_DIR}/plugins/\"*.dll"
    DEPENDS dist-lite-prepare
    COMMENT "Code signing lite distribution artifacts"
  )
else()
  add_custom_target(dist-lite-sign
    COMMAND ${CMAKE_COMMAND} -E echo "WARNING: osslsigncode not found, skipping code signing"
    DEPENDS dist-lite-prepare
    VERBATIM
  )
endif()

# Create lite tar.zst archive
add_custom_target(dist-lite-tar
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_SOURCE_DIR}/dist"
    tar --zstd -cvf "${CMAKE_SOURCE_DIR}/dist/${DIST_LITE_NAME}.tar.zst" "${DIST_LITE_NAME}"
  DEPENDS dist-lite-sign
  COMMENT "Creating ${DIST_LITE_NAME}.tar.zst"
  VERBATIM
)

# Create lite zip archive
add_custom_target(dist-lite-zip
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_SOURCE_DIR}/dist"
    ${CMAKE_COMMAND} -E tar cvf "${CMAKE_SOURCE_DIR}/dist/${DIST_LITE_NAME}.zip" --format=zip "${DIST_LITE_NAME}"
  DEPENDS dist-lite-sign
  COMMENT "Creating ${DIST_LITE_NAME}.zip"
  VERBATIM
)

# Main lite distribution target that creates both archives
add_custom_target(dist-lite
  DEPENDS dist-lite-tar dist-lite-zip
  COMMENT "Lite distribution packages created: ${DIST_LITE_NAME}.tar.zst and ${DIST_LITE_NAME}.zip"
)

# Print lite distribution info
add_custom_command(TARGET dist-lite POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
  COMMAND ${CMAKE_COMMAND} -E echo "Lite distribution packages created (stripped, no debug symbols):"
  COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_SOURCE_DIR}/dist/${DIST_LITE_NAME}.tar.zst"
  COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_SOURCE_DIR}/dist/${DIST_LITE_NAME}.zip"
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
  VERBATIM
)

# =============================================================================
# Combined dist-all target
# =============================================================================
add_custom_target(dist-all
  DEPENDS dist dist-lite
  COMMENT "All distribution packages created"
)
