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

# BugSplat64.dll, nevr-runtime.dll and echovr_server.exe carry a VERSIONINFO resource (cmake/nevr_version_info.cmake, #850): the
# resource compiler is a language, and enable_language cannot be called from a function.
if(WIN32)
  enable_language(RC)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/nevr_version_info.cmake")

# The version is plain semver from the tags: exactly X.Y.Z for a CI build on the tag vX.Y.Z, otherwise
# X.Y.(Z+1)-dev.<N>+<sha> (set_project_version_from_git.cmake).

# The version and the commit are NOT defined for the tree: a definition on every compile command rebuilds every
# translation unit, and every precompiled header, for each new commit or uncommitted edit. The few files that read
# them get them (cmake/nevr_build_identity.cmake), where those files are built.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/nevr_build_identity.cmake")

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

# ccache refuses a compile that carries CMake's C++20 module-scan flags (-fmodules-ts, -fmodule-mapper, -fdeps-*): it counts it
# uncacheable (unsupported_compiler_option). No source here is a module, so a build that goes through a ccache launcher skips the scan;
# a build without one is unchanged.
if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

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
    "$<$<CONFIG:Debug>:/Od>" # Debug: Disable optimizations
    "$<$<CONFIG:Release>:/O2>" # Release: Enable optimizations
  )
endif()

# MinGW-specific compile options
if(MINGW)
  add_compile_options(
    -Wall
    -Wextra
    -Werror
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
  # The same commit built in two directories must give the same bytes: keep the source and build
  # directories out of __FILE__, assertions and debug info.
  add_compile_options(
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
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
    # The PE header, export and debug directories carry a link-time timestamp; without this the same
    # objects link to different bytes on every run.
    -Wl,--no-insert-timestamp
  )
endif()

# Include directories
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_BINARY_DIR}
  "${CMAKE_SOURCE_DIR}/src")

# Build-time defaults embedded into the runtime (config/public-defaults.env; see
# cmake/nevr_builtin_defaults.cmake). The header lands in ${CMAKE_BINARY_DIR}/generated/.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/nevr_builtin_defaults.cmake")
nevr_generate_builtin_defaults("${CMAKE_CURRENT_SOURCE_DIR}")

# Define preprocessor macros
add_compile_definitions("$<$<CONFIG:Debug>:_DEBUG;_WINDOWS;_USRDLL>"
  "$<$<CONFIG:Release>:NDEBUG;_WINDOWS;_USRDLL>")

add_subdirectory(extern)

find_package(Protobuf REQUIRED)

# yaml-cpp (0.8.0, x64-mingw-static): backs src/core/nevr_config. Found here,
# before the src/ subdirectories, so nevr_core can see its headers (compile-only,
# SYSTEM include) and nevr_runtime can link the library. The library is linked
# ONLY into nevr_runtime (and the nevr_config test exe) — never into nevr_core's
# archive or the module DLLs, so yaml-cpp cannot leak into a module.
find_package(yaml-cpp CONFIG REQUIRED)

# The plugin SDK (sdk/CMakeLists.txt): nevr::plugin_sdk, the published plugin ABI and helpers. The runtime builds against
# the same target a plugin outside this repository links.
add_subdirectory(sdk)
add_subdirectory(src/abi)
add_subdirectory(src/core)
add_subdirectory(src/nevr_api)
add_subdirectory(src/runtime)
add_subdirectory(src/modules/platform-compat)
add_subdirectory(src/modules/token-auth)
add_subdirectory(src/reconstruction)
# There is no replacement pnsrad.dll target: ws_bridge pokes the game's ORIGINAL
# pnsrad.dll at fixed offsets (+0x88 account_id, +0x90 login state, +0x9c flags),
# and those layouts are documented there. The pure-RE copy of pnsrad lives in
# echovr-reconstruction; nothing here may symlink into a private repo, which
# breaks every clean clone.
# telemetryagent lives in nevr-runtime-plugins (extras/) — it needs a protobuf-lite
# refactor before it could be reintegrated here

add_subdirectory(src/libovr-stub)

# Server launcher — thin CreateProcess wrapper that spawns echovr.exe -server -no-console
add_subdirectory(src/launcher)

# =============================================================================
# Optional drop-in build modules (modules/*/CMakeLists.txt)
# =============================================================================
# Each subdirectory under modules/ that contains a CMakeLists.txt is
# auto-discovered here and added via add_subdirectory(). No-op when modules/
# is empty or absent. See modules/README.md.
# =============================================================================
file(GLOB NEVR_MODULE_LISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/*/CMakeLists.txt")
foreach(module_list ${NEVR_MODULE_LISTS})
  get_filename_component(module_dir "${module_list}" DIRECTORY)
  get_filename_component(module_name "${module_dir}" NAME)
  message(STATUS "[modules] adding ${module_name}")
  add_subdirectory("${module_dir}")
endforeach()

# Generic module auto-link: each module under modules/*/ self-registers its
# target by appending to NEVR_MODULE_TARGETS in its own CMakeLists.
# This block names NO specific module — the list is accumulated generically.
get_property(NEVR_MODULES GLOBAL PROPERTY NEVR_MODULE_TARGETS)
if(NEVR_MODULES)
  message(STATUS "[nevr_runtime] linking modules: ${NEVR_MODULES}")
  target_link_libraries(nevr_runtime PRIVATE ${NEVR_MODULES})
else()
  message(STATUS "[nevr_runtime] no modules to link (empty)")
endif()

# =============================================================================
# Code Signing
# =============================================================================
# Release jobs configure NEVR_CODESIGN_REQUIRED=ON. Local development defaults
# to optional unsigned output when no credentials are configured.
# =============================================================================

find_program(OSSLSIGNCODE osslsigncode)
set(CODESIGN_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/codesign/sign.sh")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
option(NEVR_CODESIGN_REQUIRED "Require trusted Authenticode signing for distributions" OFF)
set(DIST_SIGNING_ARGS)
if(NEVR_CODESIGN_REQUIRED)
  list(APPEND DIST_SIGNING_ARGS --required-signing)
endif()

# =============================================================================
# Distribution Package Target (stripped, no debug symbols)
# =============================================================================
# Creates the stripped distribution package, nevr-runtime-v<ver>-windows-server: BugSplat64.dll (the host),
# nevr-runtime.dll (the runtime) and echovr_server.exe that tools/package_release.py puts in the one Windows release zip (nevr-runtime-v<ver>-windows.zip),
# and the source of the .tar.zst workflow artifact: binaries run through strip --strip-all.
# Before stripping, the debug info of each shipped PE is split into dist/symbols/<name>/<file>.dbg and
# the stripped file gets a .gnu_debuglink to it (name and CRC32), so a crash from the field maps to
# function names through the .dbg's COFF symbol table. The release preset has no -g, so there are no source
# lines to map to. The .dbg files are never part of the package. A local build with symbols is the
# ordinary build output in build/<preset>/bin/.
# Usage: cmake --build <build_dir> --target dist
# =============================================================================

set(DIST_NAME "nevr-runtime-v${PROJECT_VERSION}-windows-server")
set(DIST_DIR "${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}")
set(DIST_STAGE_DIR "${CMAKE_SOURCE_DIR}/dist/.staging/${DIST_NAME}")
set(DIST_SYMBOLS_DIR "${CMAKE_SOURCE_DIR}/dist/symbols/${DIST_NAME}")

# Find strip tool (use llvm-strip or strip)
find_program(MINGW_STRIP_TOOL NAMES x86_64-w64-mingw32-strip llvm-strip strip)
find_program(MINGW_OBJCOPY_TOOL NAMES x86_64-w64-mingw32-objcopy llvm-objcopy objcopy)

# Custom target to prepare distribution directory with stripped DLLs
add_custom_target(dist-prepare
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_STAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_STAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_SYMBOLS_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_SYMBOLS_DIR}"
  # Per shipped PE: copy, split the debug info off, strip, link the stripped file to the .dbg.
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/BugSplat64.dll" "${DIST_STAGE_DIR}/BugSplat64.dll"
  COMMAND ${MINGW_OBJCOPY_TOOL} --only-keep-debug "${DIST_STAGE_DIR}/BugSplat64.dll" "${DIST_SYMBOLS_DIR}/BugSplat64.dll.dbg"
  COMMAND ${MINGW_STRIP_TOOL} --strip-all "${DIST_STAGE_DIR}/BugSplat64.dll"
  COMMAND ${MINGW_OBJCOPY_TOOL} "--add-gnu-debuglink=${DIST_SYMBOLS_DIR}/BugSplat64.dll.dbg" "${DIST_STAGE_DIR}/BugSplat64.dll"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/nevr-runtime.dll" "${DIST_STAGE_DIR}/nevr-runtime.dll"
  COMMAND ${MINGW_OBJCOPY_TOOL} --only-keep-debug "${DIST_STAGE_DIR}/nevr-runtime.dll" "${DIST_SYMBOLS_DIR}/nevr-runtime.dll.dbg"
  COMMAND ${MINGW_STRIP_TOOL} --strip-all "${DIST_STAGE_DIR}/nevr-runtime.dll"
  COMMAND ${MINGW_OBJCOPY_TOOL} "--add-gnu-debuglink=${DIST_SYMBOLS_DIR}/nevr-runtime.dll.dbg" "${DIST_STAGE_DIR}/nevr-runtime.dll"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:echovr_server>" "${DIST_STAGE_DIR}/echovr_server.exe"
  COMMAND ${MINGW_OBJCOPY_TOOL} --only-keep-debug "${DIST_STAGE_DIR}/echovr_server.exe" "${DIST_SYMBOLS_DIR}/echovr_server.exe.dbg"
  COMMAND ${MINGW_STRIP_TOOL} --strip-all "${DIST_STAGE_DIR}/echovr_server.exe"
  COMMAND ${MINGW_OBJCOPY_TOOL} "--add-gnu-debuglink=${DIST_SYMBOLS_DIR}/echovr_server.exe.dbg" "${DIST_STAGE_DIR}/echovr_server.exe"
  # gameserver.dll merged into nevr_runtime — no separate DLL to copy
  # COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gameserver.dll" "${DIST_DIR}/pnsradgameserver.dll" && ${STRIP_TOOL} --strip-all "${DIST_DIR}/pnsradgameserver.dll" || ${CMAKE_COMMAND} -E true
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/README.md" "${DIST_STAGE_DIR}/"
  # The tester-facing install page (#72, beta gate G5): what to install, sign-in, uninstall, logs.
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/docs/beta/INSTALL.md" "${DIST_STAGE_DIR}/INSTALL.md"
  # plugins/ ships EMPTY on purpose — it is the drop-point for optional operator
  # plugins. log_filter.dll is deliberately not packaged (N95): the loader
  # refuses it by name.
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_STAGE_DIR}/plugins"
  COMMENT "Preparing distribution directory (stripped): ${DIST_DIR}"
  VERBATIM
)
add_dependencies(dist-prepare nevr_host)
add_dependencies(dist-prepare nevr_runtime)
add_dependencies(dist-prepare echovr_server)

add_custom_target(dist-sign
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/build_distribution.py"
    --package "${DIST_STAGE_DIR}" --destination "${DIST_DIR}"
    --tar "${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}.tar.zst"
    --zip "${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}.zip" ${DIST_SIGNING_ARGS}
  DEPENDS dist-prepare
  COMMENT "Signing, verifying, and atomically publishing distribution"
  VERBATIM
)
add_custom_target(dist
  DEPENDS dist-sign
  COMMENT "Distribution packages created: ${DIST_NAME}.tar.zst and ${DIST_NAME}.zip"
)
add_custom_command(TARGET dist POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -DARCHIVE_TAR=${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}.tar.zst
    -DARCHIVE_ZIP=${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}.zip
    -P ${CMAKE_SOURCE_DIR}/cmake/VerifyDistribution.cmake
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
  COMMAND ${CMAKE_COMMAND} -E echo "Distribution packages created (stripped, no debug symbols):"
  COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}.tar.zst"
  COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_SOURCE_DIR}/dist/${DIST_NAME}.zip"
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
  COMMENT "Checking required runtime in distribution archives"
  VERBATIM
)

# =============================================================================
# Combined dist-all target
# =============================================================================
add_custom_target(dist-all
  DEPENDS dist
  COMMENT "All distribution packages created"
)
