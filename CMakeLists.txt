cmake_minimum_required(VERSION 4.0.0)

# Project Name
project(NEVRServer LANGUAGES CXX)

# import the version from the git-version.cmake file
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/set_project_version_from_git.cmake")
set_project_version_from_git()

# Print out the version that was set by set_project_version_from_git()
message(STATUS "Project version: ${PROJECT_VERSION}")
# Set the project output name
set(PROJECT_OUTPUT_NAME "${CMAKE_PROJECT_NAME}")

# Set the project description
set(PROJECT_DESCRIPTION "NEVR Game Server")

# Set the project vendor
set(PROJECT_VENDOR "EchoTools")

# Set the project organization
set(PROJECT_ORGANIZATION "EchoTools")

# Set the project URL
set(PROJECT_URL "https://github.com/echotools/nevr-server")

# set(CMAKE_BUILD_TYPE "Release")

# Check if the CMAKE_BUILD_TYPE is set, if not, default to Debug
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()
message(STATUS "Build type set to: ${CMAKE_BUILD_TYPE}")

# Sets the VCPKG feature flags for the project. "manifest" enables manifest
# mode, allowing dependencies to be managed via a vcpkg.json file.
# "no-print-usage" suppresses the printing of usage information after package
# installation.
set(VCPKG_FEATURE_FLAGS "manifest,no-print-usage")
set(ENV{VCPKG_FEATURE_FLAGS} "manifests")
set(CMAKE_VERBOSE_MAKEFILE ON)

# Set the CMake generator to Ninja
set(CMAKE_GENERATOR "Ninja")

# Set the CMake generator platform to x64
set(CMAKE_GENERATOR_PLATFORM "x64")

# Set the CMake executable linker flags
set(CMAKE_EXE_LINKER_FLAGS "/SUBSYSTEM:WINDOWS")

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add LTO for Release
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Disable UAC
add_link_options(/MANIFESTUAC:NO)

# Enforce Static Runtime Libraries
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Set the output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Enable CMake to generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add compile options for Debug and Release configurations
add_compile_options(
  "$<$<CONFIG:Debug>:/Zi>" # Debug: Enable debugging information
  "$<$<CONFIG:Debug>:/WX>" # Debug: Treat warnings as errors
  "$<$<CONFIG:Debug>:/W3>" # Debug: Warning level 3
  "$<$<CONFIG:Debug>:/sdl>" # Debug: Enable additional security checks
  "$<$<CONFIG:Debug>:/permissive->" # Debug: Enforce standard conformance
  "$<$<CONFIG:Debug>:/MTd>" # Debug: Use static runtime library (debug)
  "$<$<CONFIG:Debug>:/EHsc>" # Debug: Enable C++ exceptions
  "$<$<CONFIG:Release>:/W3>" # Release: Warning level 3
  "$<$<CONFIG:Release>:/sdl>" # Release: Enable additional security checks
  "$<$<CONFIG:Release>:/permissive->" # Release: Enforce standard conformance
  "$<$<CONFIG:Release>:/MT>" # Release: Use static runtime library
  "$<$<CONFIG:Release>:/EHsc>" # Release: Enable C++ exceptions
)

# Add link options for Debug and Release configurations
add_link_options(
  "$<$<CONFIG:Debug>:/DEBUG>" # Debug: Generate debug information
  "$<$<CONFIG:Debug>:/SUBSYSTEM:WINDOWS>" # Debug: Set subsystem to Windows
  "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>" # Release: Set subsystem to Windows
  "$<$<CONFIG:Release>:/OPT:REF>" # Release: Enable reference optimization
  "$<$<CONFIG:Release>:/OPT:ICF>" # Release: Enable identical COMDAT folding
)

add_link_options("/MANIFESTUAC:NO")

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
                    ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/common)

# Define preprocessor macros
add_compile_definitions("$<$<CONFIG:Debug>:_DEBUG;_WINDOWS;_USRDLL>"
                        "$<$<CONFIG:Release>:NDEBUG;_WINDOWS;_USRDLL>")
add_subdirectory(common)
add_subdirectory(GamePatches)
add_subdirectory(GameServer)

# Include a local cmake script if it exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/local.cmake")
endif()
