# NEVR Runtime — Quest / Android (arm64-v8a) crash-reporter sub-project.
#
# Self-contained CMake project, deliberately separate from the repo root
# CMakeLists.txt (which is hard-wired to the MinGW/MSVC + vcpkg toolchain).
# Configure with the android-arm64 preset (NDK r26d, arm64-v8a, API 26,
# static libc++).  See docs/design/2026-07-13-quest-crash-reporter-injection.md.

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

# Compile the shared login profile source with the Quest Android toolchain.
find_package(nlohmann_json CONFIG REQUIRED)
add_library(nevr_quest_login_profile STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/compat/login_profile.cpp")
target_include_directories(nevr_quest_login_profile PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_login_profile PRIVATE nlohmann_json::nlohmann_json)

# Compile the exact redirect policy source used by the Windows runtime.
add_library(nevr_quest_service_redirect STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/lifecycle/service_redirect.cpp")
target_include_directories(nevr_quest_service_redirect PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(nevr_quest_service_redirect PRIVATE -Wall -Wextra -Werror)

# Android-linkable test executable using the same vectors as the host test.
add_executable(service_redirect_test tests/service_redirect_test.cpp)
target_link_libraries(service_redirect_test PRIVATE nevr_quest_service_redirect)
target_compile_options(service_redirect_test PRIVATE -Wall -Wextra -Werror)

# Compile and link the same process-stable pool source used by PCVR.
add_library(nevr_quest_stable_string_pool STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/lifecycle/stable_string_pool.cpp")
target_include_directories(nevr_quest_stable_string_pool PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(nevr_quest_stable_string_pool PRIVATE -Wall -Wextra -Werror)

add_executable(stable_string_pool_test tests/stable_string_pool_test.cpp)
target_link_libraries(stable_string_pool_test PRIVATE nevr_quest_stable_string_pool)
target_compile_options(stable_string_pool_test PRIVATE -Wall -Wextra -Werror)

# Quest configuration and feature activation (pure; the sentinel links it, the host test compiles it).
add_library(nevr_quest_config STATIC "${CMAKE_CURRENT_SOURCE_DIR}/sentinel/quest_config.cpp")
target_include_directories(nevr_quest_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_config PUBLIC nevr_quest_service_redirect PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(nevr_quest_config PRIVATE -Wall -Wextra -Werror)

add_executable(quest_config_test tests/quest_config_test.cpp)
target_link_libraries(quest_config_test PRIVATE nevr_quest_config nlohmann_json::nlohmann_json)
target_compile_options(quest_config_test PRIVATE -Wall -Wextra -Werror)

# Build-time defaults embedded from the environment or <root>/.env at configure time (never read
# at run time). Written to the build tree only: ${CMAKE_BINARY_DIR}/generated/nevr_builtin_defaults.h.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nevr_builtin_defaults.cmake")
nevr_generate_builtin_defaults("${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ── Crash-reporter sentinel (libovrplatformloader.so) ───────────────────
# Added once breakpad is vendored; guarded so the toolchain probe builds
# standalone.
option(NEVR_QUEST_BUILD_SENTINEL "Build the breakpad crash-reporter .so" ON)
if(NEVR_QUEST_BUILD_SENTINEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sentinel/CMakeLists.txt")
  add_subdirectory(sentinel)
endif()
