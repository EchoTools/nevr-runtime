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
add_executable(nevr_quest_patch_libr15
  ../patch/quest_patch_libr15.cpp
  ../patch/libr15_patcher.cpp
  ../patch/libr15_extra_rows.cpp
  ../patch/libr15_patcher_cli.cpp
  ../patch/libr15_patch_identity.cpp
  ../data/sha256.cpp
  ../sentinel/arm64_reloc.cpp)
target_include_directories(nevr_quest_patch_libr15 PRIVATE "${NEVR_REPO_ROOT}/src" "${NEVR_REPO_ROOT}/sdk/include")
target_include_directories(nevr_quest_patch_libr15 SYSTEM PRIVATE "${NEVR_HOST_JSON_INCLUDE}")
target_compile_options(nevr_quest_patch_libr15 PRIVATE -O2 -Wall -Wextra -Werror)
# The target is nevr_<name> (docs/standards/naming.md); the file is the tool's name, which the repack recipe and the tests run.
set_target_properties(nevr_quest_patch_libr15 PROPERTIES OUTPUT_NAME quest_patch_libr15 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The host check of an extra-rows file (docs/adr/0021 section 11): the patcher's own parser on the public spec. The Quest build runs
# it at configure time (cmake/libr15_extra_rows.cmake, through this preset) so a file the patcher would refuse stops the
# sentinel's build instead of surfacing on a headset.
add_executable(nevr_quest_libr15_extra_rows_check
  ../patch/libr15_extra_rows_check.cpp
  ../patch/libr15_extra_rows.cpp
  ../patch/libr15_patch_identity.cpp
  ../data/sha256.cpp)
target_include_directories(nevr_quest_libr15_extra_rows_check PRIVATE "${NEVR_REPO_ROOT}/src" "${NEVR_REPO_ROOT}/sdk/include")
target_include_directories(nevr_quest_libr15_extra_rows_check SYSTEM PRIVATE "${NEVR_HOST_JSON_INCLUDE}")
target_compile_options(nevr_quest_libr15_extra_rows_check PRIVATE -O1)
set_target_properties(nevr_quest_libr15_extra_rows_check PROPERTIES OUTPUT_NAME libr15_extra_rows_check RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The store OBB extractor (docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md): `just quest-stock-data` builds it with the
# quest-obb-extract preset and runs it; the host tests (host_tests/groups/obb.cmake) link the same library. nettle comes from the
# tools/quest-host-deps manifest, installed by `just _vcpkg-quest-host` into NEVR_HOST_PREFIX; never a system library, whose
# version (and function types) can differ. Off in the other host-tools trees, so a tree configured without that install still builds.
option(NEVR_QUEST_OBB_EXTRACT "Build the store OBB extractor (needs nettle from NEVR_HOST_PREFIX)" OFF)
set(NEVR_HOST_PREFIX "" CACHE PATH "The x64-linux vcpkg tree of tools/quest-host-deps (the quest-* host presets set it)")
if(NEVR_QUEST_OBB_EXTRACT)
  find_library(NEVR_NETTLE_LIB NAMES libnettle.a PATHS "${NEVR_HOST_PREFIX}/lib" NO_DEFAULT_PATH)
  if(NOT NEVR_NETTLE_LIB OR NOT EXISTS "${NEVR_HOST_PREFIX}/include/nettle/twofish.h")
    message(FATAL_ERROR
      "nevr_quest host tools: nettle (libnettle.a and nettle/twofish.h) is not under NEVR_HOST_PREFIX='${NEVR_HOST_PREFIX}'; "
      "'just _vcpkg-quest-host' installs it from tools/quest-host-deps")
  endif()
  add_library(nevr_quest_obb STATIC
    ../obb/byte_source.cpp
    ../obb/obb_footer.cpp
    ../obb/obb_key.cpp
    ../obb/obb_cipher.cpp
    ../obb/fat16.cpp
    ../obb/stock_pins.cpp
    ../obb/build_log.cpp
    ../obb/stock_data.cpp
    ../data/sha256.cpp)
  target_include_directories(nevr_quest_obb PUBLIC "${NEVR_REPO_ROOT}/src")
  # -isystem puts the pinned nettle ahead of any system copy. _GLIBCXX_ASSERTIONS: the tool reads a file it did not write; an
  # out-of-range operator[] aborts instead of reading past the buffer.
  target_include_directories(nevr_quest_obb SYSTEM PUBLIC "${NEVR_HOST_PREFIX}/include" "${NEVR_HOST_JSON_INCLUDE}")
  target_compile_definitions(nevr_quest_obb PUBLIC _GLIBCXX_ASSERTIONS)
  target_compile_options(nevr_quest_obb PRIVATE -O2 -Wall -Wextra -Werror)
  target_link_libraries(nevr_quest_obb PUBLIC "${NEVR_NETTLE_LIB}")

  add_executable(nevr_quest_obb_extract ../obb/obb_extract_main.cpp)
  target_link_libraries(nevr_quest_obb_extract PRIVATE nevr_quest_obb)
  target_compile_options(nevr_quest_obb_extract PRIVATE -O2 -Wall -Wextra -Werror)
  set_target_properties(nevr_quest_obb_extract PROPERTIES OUTPUT_NAME quest_obb_extract RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# The host tests of the `just test-quest-*` recipes (docs/adr/0041): the quest-host-* presets turn this on, besides the host tools.
if(NEVR_QUEST_HOST_TESTS)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../host_tests" host_tests)
endif()
