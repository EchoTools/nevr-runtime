# Host tools of the Quest build: programs that run on the machine that builds the APK, not on the headset. Reached only through
# the quest-patcher preset (src/quest/CMakePresets.json, NEVR_QUEST_HOST_TOOLS=ON), which names the host compiler; the NDK is
# not involved. docs/adr/0041-native-code-is-built-by-cmake.md: a host tool is a target, and a recipe builds it with
# `cmake --build --preset ... --target`.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# -std=c++17, the spelling the recipe this replaced used, not -std=gnu++17.
set(CMAKE_CXX_EXTENSIONS OFF)
# No module scan: no source is a module, and a build through a ccache launcher cannot cache a scanned compile (as src/quest).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# The directory that holds nlohmann/json.hpp. `just quest-patcher` passes it; the preset's default is the tree
# `just _vcpkg-quest-host` installs it into from tools/quest-host-deps (never a system package).
set(NEVR_HOST_JSON_INCLUDE "" CACHE PATH "Directory that holds nlohmann/json.hpp, for the Quest host tools")
if(NOT EXISTS "${NEVR_HOST_JSON_INCLUDE}/nlohmann/json.hpp")
  message(FATAL_ERROR
    "nevr_quest host tools: nlohmann/json.hpp is not under NEVR_HOST_JSON_INCLUDE='${NEVR_HOST_JSON_INCLUDE}'; "
    "'just _vcpkg-quest-host' installs it, or pass json_include to 'just quest-patcher'")
endif()

# The libr15 build-time patcher (docs/adr/0021): `android-repack-apk` runs it on the store's libr15.so, and the host tests
# (`just test-quest-patcher`, `test-quest-patcher-pinned`) run it. It lands in the build directory itself:
# build/quest-patcher/quest_patch_libr15.
add_executable(quest_patch_libr15
  ../patch/quest_patch_libr15.cpp
  ../patch/libr15_patcher.cpp
  ../patch/libr15_extra_rows.cpp
  ../patch/libr15_patcher_cli.cpp
  ../patch/libr15_patch_identity.cpp
  ../data/sha256.cpp
  ../sentinel/arm64_reloc.cpp)
target_include_directories(quest_patch_libr15 PRIVATE "${NEVR_REPO_ROOT}/src" "${NEVR_REPO_ROOT}/sdk/include")
target_include_directories(quest_patch_libr15 SYSTEM PRIVATE "${NEVR_HOST_JSON_INCLUDE}")
target_compile_options(quest_patch_libr15 PRIVATE -O2 -Wall -Wextra -Werror)
set_target_properties(quest_patch_libr15 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
