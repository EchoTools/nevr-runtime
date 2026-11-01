# NEVR Runtime — Quest / Android (arm64-v8a) crash-reporter sub-project.
#
# Self-contained CMake project, deliberately separate from the repo root
# CMakeLists.txt (which is hard-wired to the MinGW/MSVC + vcpkg toolchain).
# Configure with the android-arm64 preset (NDK r26d, arm64-v8a, API 26,
# static libc++).  See docs/design/2026-07-13-quest-crash-reporter-injection.md.

cmake_minimum_required(VERSION 3.23)
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

# ccache refuses a compile that carries CMake's C++20 module-scan flags (-fmodules-ts, -fmodule-mapper, -fdeps-*): it counts it
# uncacheable. No source here is a module, so a build that goes through a ccache launcher (every checkout, #1736) skips the
# scan, as the root CMakeLists.txt does; a build without a launcher is unchanged.
if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

# The same commit built in two worktrees must give the same bytes (#658): keep the repository root and the build
# directory out of __FILE__, assertions and debug info, as the root CMakeLists.txt does for the Windows DLLs. The
# build tree is <repo>/build/<preset>, so its mapping is relative too.
get_filename_component(NEVR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_compile_options(
  "-ffile-prefix-map=${NEVR_REPO_ROOT}=."
  "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
)

message(STATUS "nevr_quest: ABI=${ANDROID_ABI} platform=${ANDROID_PLATFORM} STL=${ANDROID_STL}")

# The extra rows of a variant build (docs/adr/0021 section 11): a file of word-patch rows, the same file the libr15 patcher is
# given (`android-repack-apk ... extra_rows=<file>`). Empty is the public build: no parser is linked and no row is carried. A
# relative path is taken from the repository root. A file the patcher would refuse is a configure error
# (cmake/libr15_extra_rows.cmake). The file is supplied by whoever builds the variant; it is never committed.
set(NEVR_LIBR15_EXTRA_ROWS "" CACHE FILEPATH "Word-patch rows for a variant build (empty: the public build)")

# The plugin SDK (sdk/CMakeLists.txt): nevr::plugin_sdk, the plugin C ABI and the CSymbol64 hash. The Quest loader and
# its plugins build against the target a plugin outside this repository links.
add_subdirectory("${NEVR_REPO_ROOT}/sdk" "${CMAKE_BINARY_DIR}/nevr_plugin_sdk")

# ── Toolchain probe ─────────────────────────────────────────────────────
# Trivial .so that compiles + links green, proving the NDK path works before
# any breakpad/sentinel code is involved (task #1 acceptance).
add_library(nevr_toolchain_probe SHARED probe.cpp)
target_link_libraries(nevr_toolchain_probe PRIVATE log)

# Compile the shared login profile source with the Quest Android toolchain.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)  # the friend-name profile reply is a zstd frame (runtime/compat/social_names.cpp)
add_library(nevr_quest_login_profile STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/compat/login_profile.cpp")
target_include_directories(nevr_quest_login_profile PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_login_profile PRIVATE nlohmann_json::nlohmann_json)

# Compile the exact EVR codec source used by the Windows runtime (no Windows headers, no sockets).
add_library(nevr_quest_evr_codec STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/compat/evr_codec.cpp")
target_include_directories(nevr_quest_evr_codec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_evr_codec PUBLIC nevr::plugin_sdk)
target_compile_options(nevr_quest_evr_codec PRIVATE -Wall -Wextra -Werror)

# The self-check unit (runtime/compat/self_check.cpp): the registry and the remote log frame a run-card check's
# result goes out in. No Windows headers, no sockets; the same source the Windows runtime compiles.
add_library(nevr_quest_self_check STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/compat/self_check.cpp")
target_include_directories(nevr_quest_self_check PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_self_check PUBLIC nevr_quest_evr_codec nlohmann_json::nlohmann_json)
target_compile_options(nevr_quest_self_check PRIVATE -Wall -Wextra -Werror)

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

# Quest social provider: a CNSISocial-compatible facade over the shared PCVR social model, the frame
# observer that feeds it and the hook installer (docs/adr/0003, "Social provider"). The sentinel links it
# through nevr_quest_integration, which owns activation.
add_library(nevr_quest_social STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/social/social_facade.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/social_frames.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/social_game_calls.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/social_install.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/social_invite_gate.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/lobby_full_hooks.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/menu_version_label.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sentinel/splash_skip_hook.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/compat/social_names.cpp")
# social_names.cpp is the PC's friend-name decoder. The sentinel may carry no dynamic initializer, so the
# static registration the PC uses is compiled out and InstallSocialHook registers the decoder explicitly.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../runtime/compat/social_names.cpp"
  PROPERTIES COMPILE_DEFINITIONS "NEVR_SOCIAL_NAMES_NO_STATIC_REGISTRATION")
# The frames a game call passes through (the Update/Join slots that call the game's callbacks and the
# Social() handler) are built without exception support, as callback_thunk.h requires; the rest of the
# facade keeps exceptions so it can contain its own failures. tools/check_quest_social_frames.sh pins it.
set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/social/social_game_calls.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/social_install.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/social_invite_gate.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/lobby_full_hooks.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/social/menu_version_label.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sentinel/splash_skip_hook.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/social_install_test.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_include_directories(nevr_quest_social PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# menu_version_label.cpp includes generated/nevr_build_info.h, which nevr_generate_build_info writes to the build tree.
target_include_directories(nevr_quest_social PRIVATE "${CMAKE_BINARY_DIR}")
target_link_libraries(nevr_quest_social PUBLIC nevr_quest_got_hook nevr::plugin_sdk dl PRIVATE zstd::libzstd nlohmann_json::nlohmann_json)
target_compile_options(nevr_quest_social PRIVATE -Wall -Wextra -Werror)

foreach(social_test social_facade_test social_install_test social_names_test)
  add_executable(${social_test} "tests/${social_test}.cpp")
  target_link_libraries(${social_test} PRIVATE nevr_quest_social)
  target_compile_options(${social_test} PRIVATE -Wall -Wextra -Werror)
endforeach()

# Quest configuration and feature activation (the sentinel links it, the host test compiles it). The file is
# config.yaml, parsed by the SAME nevr_config source the PC compiles (src/core/nevr_config.cpp, yaml-cpp); the
# PC-only half (nevr_config_policy.cpp: the N4 wrapper, the logged warning) is not built here, and nevr_config.cpp
# includes no PC logger. Both are built WITH exceptions: quest_config.cpp catches every one by name, and nothing on
# the hook path (entry.cpp, login_prompt_hook.cpp, obb_skip_hook.cpp) is built that way.
find_package(yaml-cpp CONFIG REQUIRED)
add_library(nevr_quest_config STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/sentinel/quest_config.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../core/nevr_config.cpp")
target_include_directories(nevr_quest_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_config PUBLIC nevr_quest_service_redirect PRIVATE nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp)
target_compile_options(nevr_quest_config PRIVATE -Wall -Wextra -Werror)

add_executable(quest_config_test tests/quest_config_test.cpp)
target_link_libraries(quest_config_test PRIVATE nevr_quest_config nlohmann_json::nlohmann_json)
target_compile_options(quest_config_test PRIVATE -Wall -Wextra -Werror)

# Config-string redirect (CJson::TString hook adapter and decision logic).
add_subdirectory(redirect)

# Build-time defaults embedded from <root>/config/public-defaults.env at configure time (never read
# at run time). Written to the build tree only: ${CMAKE_BINARY_DIR}/generated/nevr_builtin_defaults.h.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nevr_builtin_defaults.cmake")
nevr_generate_builtin_defaults("${CMAKE_CURRENT_SOURCE_DIR}/../..")
# The version (with its commit).
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nevr_build_info.cmake")
nevr_generate_build_info("${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ── Token auth (device-code login, refresh, cache) ──────────────────────
# The platform-neutral core (src/core) is the SAME source the Windows runtime
# compiles; src/quest/auth is the Android adapter set (libcurl over OpenSSL from
# the Quest vcpkg manifest, the cache file, the login-link file, a real clock).
# Linked into the sentinel through nevr_quest_integration (the token feeds the login rewrite).
find_package(CURL CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
# curl_http.cpp needs CURLOPT_PROTOCOLS_STR (7.85) and CURLOPT_QUICK_EXIT (7.87). The Quest manifest
# has no builtin-baseline (neither does the root one), so what vcpkg resolves is whatever the
# local vcpkg checkout carries; fail the configure rather than the compile when it is too old.
if(NOT CURL_VERSION OR CURL_VERSION VERSION_LESS 7.87.0)
  message(FATAL_ERROR "nevr_quest: libcurl ${CURL_VERSION} found, need >= 7.87.0 (CURLOPT_QUICK_EXIT)")
endif()
message(STATUS "nevr_quest: libcurl ${CURL_VERSION}")
# The sign-in prompt shared by token auth (the writer) and the sentinel's login-prompt hook (the
# reader, on the game's call path). Built without exceptions so the reader's frames carry no
# personality (docs/adr/0003, hook contract rule 5); it allocates nothing and does not log.
add_library(nevr_quest_prompt_board STATIC auth/prompt_board.cpp)
target_include_directories(nevr_quest_prompt_board PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(nevr_quest_prompt_board PRIVATE
  -Wall -Wextra -Werror -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden)

# The temp-file/fsync/rename writer shared by the credential cache and the hardware dump.
add_library(nevr_quest_atomic_write STATIC auth/atomic_write.cpp)
target_include_directories(nevr_quest_atomic_write PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(nevr_quest_atomic_write PRIVATE -Wall -Wextra -Werror)

# The first-run game-data step's core (docs/adr/0011-quest-game-data-first-run.md): sha256, the zip extractor, the
# pins and decision, the staging swap and the fetch state machine. Plain POSIX plus zlib; built without exceptions (it runs on the game's thread).
find_package(ZLIB REQUIRED)
add_library(nevr_quest_data STATIC data/sha256.cpp data/zip.cpp data/staging.cpp data/fetch.cpp data/data_step.cpp data/pack.cpp)
target_include_directories(nevr_quest_data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_data PUBLIC ZLIB::ZLIB PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(nevr_quest_data PRIVATE -Wall -Wextra -Werror -fno-exceptions)

add_library(nevr_quest_token_auth STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../core/auth_refresh.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../core/device_auth_flow.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../core/device_poll_response.cpp"
  auth/session.cpp
  auth/file_store.cpp
  auth/prompt_presenters.cpp
  auth/ca_bundle.cpp
  auth/curl_http.cpp
  auth/quest_token_auth.cpp)
target_include_directories(nevr_quest_token_auth PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(nevr_quest_token_auth PUBLIC nevr_quest_atomic_write nlohmann_json::nlohmann_json CURL::libcurl OpenSSL::Crypto
  nevr_quest_prompt_board)
target_compile_options(nevr_quest_token_auth PRIVATE -Wall -Wextra -Werror)

# The libcurl Transport the data fetch downloads through; its trust anchors are the roots in data/data_host_roots.h.
add_library(nevr_quest_data_net STATIC data/curl_transport.cpp data/data_host_roots.cpp)
target_link_libraries(nevr_quest_data_net PUBLIC nevr_quest_data CURL::libcurl)
target_compile_options(nevr_quest_data_net PRIVATE -Wall -Wextra -Werror -fno-exceptions)

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

# The login hook links the shared GOT backend target built by the sentinel (a second copy of
# the backend source would carry its own write lock and slot registry).
if(TARGET nevr_quest_got_hook)
  # Quest login rewrite: portable decision code + the Android adapter that hooks
  # CNSUser::SendLogInRequest in libpnsovr.so through the GOT import hook backend.
  add_library(nevr_quest_login STATIC
    login/login_rewrite.cpp
    login/login_hook.cpp
    login/login_counters.cpp
    login/login_apply.cpp
    login/login_prerequisites.cpp
    login/login_prerequisites_install.cpp
    login/login_standin.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/compat/login_profile.cpp")
  target_include_directories(nevr_quest_login PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_link_libraries(nevr_quest_login PUBLIC nlohmann_json::nlohmann_json PRIVATE nevr_quest_got_hook log dl)
  target_compile_options(nevr_quest_login PRIVATE -Wall -Wextra -Werror)
  # Every frame that is live while game code runs is built without exception support, so it
  # carries no personality or LSDA (the contract in sentinel/callback_thunk.h): the thunk
  # handler and install code (login_hook.cpp) and everything that calls the game
  # (login_apply.cpp), and the login-prerequisite handlers and their install
  # (login_prerequisites.cpp, login_prerequisites_install.cpp), which run inside the game's OVR
  # callbacks. login_rewrite.cpp keeps exceptions; it calls no game code and returns before the
  # next game call.
  set_source_files_properties(login/login_hook.cpp login/login_counters.cpp login/login_apply.cpp login/login_prerequisites.cpp
    login/login_prerequisites_install.cpp login/login_standin.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")

  # Android-linkable build of the host test (cannot execute on the build machine).
  add_executable(login_rewrite_test tests/login_rewrite_test.cpp login/login_standin.cpp)
  target_link_libraries(login_rewrite_test PRIVATE nevr_quest_login)
  target_compile_options(login_rewrite_test PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(login_rewrite_test PRIVATE NEVR_QUEST_TESTING)

  # Install-code host test (partial install, idempotency, gating). Linked against the whole login
  # archive so the install, its thunks and the backend are present.
  # The install is compiled into the test as well, with the test-only macro, so the test's slot seam (login_prerequisites.h) exists;
  # the library's copy has none.
  add_executable(login_prerequisites_install_test tests/login_prerequisites_install_test.cpp login/login_prerequisites_install.cpp)
  target_link_libraries(login_prerequisites_install_test PRIVATE nevr_quest_login)
  target_compile_definitions(login_prerequisites_install_test PRIVATE NEVR_QUEST_TESTING)
  # -fno-exceptions: the test arms the entitlement hook's thunk (callback_thunk.h refuses other units).
  target_compile_options(login_prerequisites_install_test PRIVATE -Wall -Wextra -Werror -fno-exceptions)

  add_executable(login_standin_test tests/login_standin_test.cpp login/login_standin.cpp)
  target_link_libraries(login_standin_test PRIVATE nevr_quest_login)
  target_compile_options(login_standin_test PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(login_standin_test PRIVATE NEVR_QUEST_TESTING)

  # Never run: the frames sensor in tests/quest reads it. It links the whole archive so the
  # login handler and everything it calls are present at final addresses.
  add_executable(login_frames_probe tests/login_frames_probe.cpp)
  target_link_libraries(login_frames_probe PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,nevr_quest_login>")
  target_compile_options(login_frames_probe PRIVATE -Wall -Wextra -Werror)

endif()

# ── Session router + loopback transport (ADR 0003, contract 3) ───────────
# Kept in its own subdirectory so it merges cleanly alongside the other Quest packages.
add_subdirectory(net)

# ── Invite service (docs/adr/0017, #966): the PC's invite_service.cpp for the Quest, linked by the sentinel integration and the link test ──
add_subdirectory(invite)

# ── Sentinel integration (ADR 0003, "Integration") ───────────────────────
# Wires every package above into the sentinel entry point. After net/, login, social and the redirect,
# because it links all of them.
if(TARGET ovrplatformloader)
  # The hardware/environment dump (#335), linked through the integration.
  add_subdirectory(diag)
  add_subdirectory(integration)
endif()
