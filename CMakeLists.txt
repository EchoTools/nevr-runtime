cmake_minimum_required(VERSION 3.20)

# Project Name
project(NEVRServer LANGUAGES CXX)

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
  GIT_COMMIT_HASH="${GIT_COMMIT_HASH}")

# Print out the version that was set by set_project_version_from_git()
message(STATUS "Project version: ${PROJECT_VERSION} (${GIT_COMMIT_HASH})")

# Set the project output name
set(PROJECT_OUTPUT_NAME "${CMAKE_PROJECT_NAME}")

# Set the project description
set(PROJECT_DESCRIPTION "NEVR Game Server")

# Set the project vendor
set(PROJECT_VENDOR "EchoTools")

# Set the project URL
set(PROJECT_URL "https://github.com/echotools/nevr-server")

# Check if the CMAKE_BUILD_TYPE is set, if not, default to Debug
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

message(STATUS "Build type set to: ${CMAKE_BUILD_TYPE}")

set(CMAKE_VERBOSE_MAKEFILE ON)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-specific settings
if(MSVC)
  # MSVC settings for Windows native builds
  set(CMAKE_EXE_LINKER_FLAGS "/SUBSYSTEM:WINDOWS")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  add_link_options(/MANIFESTUAC:NO)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  
  add_compile_options(
    "/Zi" # Enable debugging information
    "/W3" # Warning level 3
    "/sdl" # Enable additional security checks
    "/permissive-" # Enforce standard conformance
    "/MT" # Use static runtime library
    "/EHsc" # Enable C++ exceptions
    "$<$<CONFIG:Debug>:/D_DEBUG>"
    "$<$<CONFIG:Release>:/DNDEBUG>"
  )
  
  add_link_options(
    "$<$<CONFIG:Debug>:/DEBUG>"
    "$<$<CONFIG:Release>:/DEBUG>"
    "$<$<CONFIG:Release>:/OPT:REF>"
    "$<$<CONFIG:Release>:/OPT:ICF>"
  )
endif()

if(MINGW)
  # MinGW cross-compilation settings
  add_compile_options(
    -Wall
    -Wextra
    "$<$<CONFIG:Debug>:-g>"
    "$<$<CONFIG:Debug>:-O0>"
    "$<$<CONFIG:Release>:-O3>"
    "$<$<CONFIG:Release>:-DNDEBUG>"
  )
  
  add_link_options(
    -mwindows
    -static
    -static-libgcc
    -static-libstdc++
  )
endif()

# Set the output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Enable CMake to generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include directories
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_BINARY_DIR}
  "${CMAKE_SOURCE_DIR}/common" "${CMAKE_SOURCE_DIR}/extern/nevr-common/common")

# Define preprocessor macros
add_compile_definitions("$<$<CONFIG:Debug>:_DEBUG;_WINDOWS;_USRDLL>"
  "$<$<CONFIG:Release>:NDEBUG;_WINDOWS;_USRDLL>")

set(NEVR_COMMON_INCLUDE "${CMAKE_SOURCE_DIR}/extern/nevr-common/common")

# Include MinHook (external dependencies)
add_subdirectory(extern/minhook)

find_package(Protobuf REQUIRED)

# Add all modules from main
add_subdirectory(common)
add_subdirectory(ProtobufNEVR)
add_subdirectory(GamePatches)
add_subdirectory(GamePatchesLegacy)
add_subdirectory(GameServer)
add_subdirectory(GameServerLegacy)
add_subdirectory(Supervisor)
add_subdirectory(TelemetryAgent)

# Include a local cmake script if it exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/local.cmake")
endif()

# =============================================================================
# Distribution Package Target
# =============================================================================

set(DIST_NAME "nevr-server-v${PROJECT_VERSION}")
set(DIST_DIR "${CMAKE_BINARY_DIR}/dist/${DIST_NAME}")

# List of DLL artifacts to include
set(DIST_DLLS
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GamePatches.dll"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GamePatchesLegacy.dll"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GameServer.dll"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/GameServerLegacy.dll"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/TelemetryAgent.dll"
)

# List of debug symbol files to include
set(DIST_DBG_FILES
  "${CMAKE_BINARY_DIR}/GamePatches.dll.dbg"
  "${CMAKE_BINARY_DIR}/GamePatchesLegacy.dll.dbg"
  "${CMAKE_BINARY_DIR}/GameServer.dll.dbg"
  "${CMAKE_BINARY_DIR}/GameServerLegacy.dll.dbg"
  "${CMAKE_BINARY_DIR}/TelemetryAgent.dll.dbg"
)

# PowerShell supervisor script
set(DIST_SCRIPTS
  "${CMAKE_BINARY_DIR}/NEVRSupervisor.ps1"
)

# Custom target to prepare distribution directory
add_custom_target(dist-prepare
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/dist"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy ${DIST_DLLS} "${DIST_DIR}/"
  COMMAND ${CMAKE_COMMAND} -E copy ${DIST_DBG_FILES} "${DIST_DIR}/"
  COMMAND ${CMAKE_COMMAND} -E copy ${DIST_SCRIPTS} "${DIST_DIR}/"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/README.md" "${DIST_DIR}/"
  DEPENDS GamePatches GamePatchesLegacy GameServer GameServerLegacy TelemetryAgent Supervisor
  COMMENT "Preparing distribution directory: ${DIST_DIR}"
  VERBATIM
)

# Create tar.zst archive
add_custom_target(dist-tar
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/dist"
    tar --zstd -cvf "${CMAKE_BINARY_DIR}/dist/${DIST_NAME}.tar.zst" "${DIST_NAME}"
  DEPENDS dist-prepare
  COMMENT "Creating ${DIST_NAME}.tar.zst"
  VERBATIM
)

# Create zip archive
add_custom_target(dist-zip
  COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/dist"
    ${CMAKE_COMMAND} -E tar cvf "${CMAKE_BINARY_DIR}/dist/${DIST_NAME}.zip" --format=zip "${DIST_NAME}"
  DEPENDS dist-prepare
  COMMENT "Creating ${DIST_NAME}.zip"
  VERBATIM
)

# Main distribution target
add_custom_target(dist
  DEPENDS dist-tar dist-zip
  COMMENT "Distribution packages created: ${DIST_NAME}.tar.zst and ${DIST_NAME}.zip"
)

add_custom_command(TARGET dist POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
  COMMAND ${CMAKE_COMMAND} -E echo "Distribution packages created:"
  COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_BINARY_DIR}/dist/${DIST_NAME}.tar.zst"
  COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_BINARY_DIR}/dist/${DIST_NAME}.zip"
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
  VERBATIM
)
