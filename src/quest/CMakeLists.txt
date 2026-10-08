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

# Compile the exact EVR codec source used by the Windows runtime (no Windows headers, no sockets).
add_library(nevr_quest_evr_codec STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/compat/evr_codec.cpp")
target_include_directories(nevr_quest_evr_codec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(nevr_quest_evr_codec PRIVATE -Wall -Wextra -Werror)

# Android-linkable codec test; the same file runs on the host in `just test-quest-shared`.
add_executable(evr_codec_test tests/evr_codec_test.cpp)
target_link_libraries(evr_codec_test PRIVATE nevr_quest_evr_codec nevr_quest_login_profile nlohmann_json::nlohmann_json)
target_include_directories(evr_codec_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(evr_codec_test PRIVATE -Wall -Wextra -Werror)

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

# ── Token auth (device-code login, refresh, cache) ──────────────────────
# The platform-neutral core (src/core) is the SAME source the Windows runtime
# compiles; src/quest/auth is the Android adapter set (libcurl over OpenSSL from
# the Quest vcpkg manifest, the cache file, the login-link file, a real clock).
# Not linked into the sentinel yet: the hook that hands the token to the login
# path is gated by ADR 0003.
find_package(CURL CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
# curl_http.cpp needs CURLOPT_PROTOCOLS_STR (7.85) and CURLOPT_QUICK_EXIT (7.87). The Quest manifest
# has no builtin-baseline (neither does the root one), so what vcpkg resolves is whatever the
# local vcpkg checkout carries; fail the configure rather than the compile when it is too old.
if(NOT CURL_VERSION OR CURL_VERSION VERSION_LESS 7.87.0)
  message(FATAL_ERROR "nevr_quest: libcurl ${CURL_VERSION} found, need >= 7.87.0 (CURLOPT_QUICK_EXIT)")
endif()
message(STATUS "nevr_quest: libcurl ${CURL_VERSION}")
add_library(nevr_quest_token_auth STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../core/auth_refresh.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../core/device_auth_flow.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../core/device_poll_response.cpp"
  auth/session.cpp
  auth/file_store.cpp
  auth/ca_bundle.cpp
  auth/curl_http.cpp
  auth/quest_token_auth.cpp)
target_include_directories(nevr_quest_token_auth PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_token_auth PUBLIC nlohmann_json::nlohmann_json CURL::libcurl OpenSSL::Crypto)
target_compile_options(nevr_quest_token_auth PRIVATE -Wall -Wextra -Werror)

add_executable(auth_core_test tests/auth_core_test.cpp)
target_link_libraries(auth_core_test PRIVATE nevr_quest_token_auth)
target_compile_options(auth_core_test PRIVATE -Wall -Wextra -Werror)

# ── Crash-reporter sentinel (libovrplatformloader.so) ───────────────────
# Added once breakpad is vendored; guarded so the toolchain probe builds
# standalone.
option(NEVR_QUEST_BUILD_SENTINEL "Build the breakpad crash-reporter .so" ON)
if(NEVR_QUEST_BUILD_SENTINEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sentinel/CMakeLists.txt")
  add_subdirectory(sentinel)
endif()

# ── Session router + loopback transport (ADR 0003, contract 3) ───────────
# Kept in its own subdirectory so it merges cleanly alongside the other Quest packages.
add_subdirectory(net)
