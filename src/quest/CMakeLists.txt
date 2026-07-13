# NEVR Runtime — Quest / Android (arm64-v8a) crash-reporter sub-project.
#
# Self-contained CMake project, deliberately separate from the repo root
# CMakeLists.txt (which is hard-wired to the MinGW/MSVC + vcpkg toolchain).
# Configure with the android-arm64 preset (NDK r26d, arm64-v8a, API 26,
# static libc++).  See docs/2026-07-13-quest-crash-reporter-injection.md.

cmake_minimum_required(VERSION 3.20)
project(nevr_quest LANGUAGES C CXX)

if(NOT ANDROID)
  message(FATAL_ERROR
    "src/quest is an Android target. Configure with the android-arm64 preset:\n"
    "  cd src/quest && cmake --preset android-arm64\n"
    "(root of the repo builds the Windows DLLs; this sub-project builds the .so)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

message(STATUS "nevr_quest: ABI=${ANDROID_ABI} platform=${ANDROID_PLATFORM} STL=${ANDROID_STL}")

# ── Toolchain probe ─────────────────────────────────────────────────────
# Trivial .so that compiles + links green, proving the NDK path works before
# any breakpad/sentinel code is involved (task #1 acceptance).
add_library(nevr_toolchain_probe SHARED probe.cpp)
target_link_libraries(nevr_toolchain_probe PRIVATE log)

# ── Crash-reporter sentinel (libovrplatformloader.so) ───────────────────
# Added once breakpad is vendored; guarded so the toolchain probe builds
# standalone.
option(NEVR_QUEST_BUILD_SENTINEL "Build the breakpad crash-reporter .so" ON)
if(NEVR_QUEST_BUILD_SENTINEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sentinel/CMakeLists.txt")
  add_subdirectory(sentinel)
endif()
