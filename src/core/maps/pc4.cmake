# CDN_DESIGN PC4 (docs/adr/0047): the channel manifest and package index parsers (core/map_manifest.h) and the minisign
# verifier (core/minisign_verify.h) in nevr_core, plus their test. Included through maps/maps.cmake from
# src/core/CMakeLists.txt, after nevr_core is defined; paths are absolute because include() keeps the including directory.
# The body sha256 comes from the portable streaming SHA-256 the Quest data step already ships (quest/data/sha256.cpp).
set(_pc4_src "${CMAKE_SOURCE_DIR}/src")
set(_pc4_monocypher "${CMAKE_SOURCE_DIR}/third_party/monocypher/src")
set(PC4_SOURCES "${_pc4_src}/core/map_manifest.cpp" "${_pc4_src}/core/minisign_verify.cpp"
  "${_pc4_src}/quest/data/sha256.cpp")
set(PC4_MONOCYPHER "${_pc4_monocypher}/monocypher.c" "${_pc4_monocypher}/monocypher-ed25519.c")

# Vendored unmodified (third_party/monocypher/UPSTREAM.txt), so its warnings are not ours to fix under -Werror.
if(MSVC)
  set_source_files_properties(${PC4_MONOCYPHER} PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(${PC4_MONOCYPHER} PROPERTIES COMPILE_OPTIONS "-w")
endif()
# The C files skip it too: nevr_core's precompiled header is C++ (pch.h includes <cstdio>).
set_source_files_properties(${PC4_SOURCES} ${PC4_MONOCYPHER} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_sources(nevr_core PRIVATE ${PC4_SOURCES} ${PC4_MONOCYPHER})
target_include_directories(nevr_core PRIVATE "${_pc4_monocypher}")

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # Signs its manifests at run time with fixed-seed test keys, and cross-checks one minisign-made signature
  # (tests/vectors/maps/minisign). Reads only tests/vectors/maps; no network, no game file.
  add_executable(map_manifest_test "${_pc4_src}/core/tests/map_manifest_test.cpp")
  set_source_files_properties("${_pc4_src}/core/tests/map_manifest_test.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_compile_definitions(map_manifest_test PRIVATE
    NEVR_MAP_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/maps")
  target_include_directories(map_manifest_test PRIVATE "${_pc4_src}" "${_pc4_monocypher}")
  target_link_libraries(map_manifest_test PRIVATE GTest::gtest GTest::gtest_main nevr_core nlohmann_json::nlohmann_json)
  include(GoogleTest)
  gtest_discover_tests(map_manifest_test DISCOVERY_MODE PRE_TEST)
endif()
