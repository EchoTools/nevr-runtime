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
    login/login_apply.cpp
    login/login_prerequisites.cpp
    login/login_prerequisites_install.cpp
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
  set_source_files_properties(login/login_hook.cpp login/login_apply.cpp login/login_prerequisites.cpp
    login/login_prerequisites_install.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")

  # Android-linkable build of the host test (cannot execute on the build machine).
  add_executable(login_rewrite_test tests/login_rewrite_test.cpp)
  target_link_libraries(login_rewrite_test PRIVATE nevr_quest_login)
  target_compile_options(login_rewrite_test PRIVATE -Wall -Wextra -Werror)

  # Never run: the frames sensor in tests/quest reads it. It links the whole archive so the
  # login handler and everything it calls are present at final addresses.
  add_executable(login_frames_probe tests/login_frames_probe.cpp)
  target_link_libraries(login_frames_probe PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,nevr_quest_login>")
  target_compile_options(login_frames_probe PRIVATE -Wall -Wextra -Werror)

endif()

