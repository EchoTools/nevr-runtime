# CDN_DESIGN PC10 (docs/adr/0047): the recipe rebuild (core/map_derive.h), in nevr_core for the PC runtime, and its test.
# Included through maps/maps.cmake from src/core/CMakeLists.txt, after nevr_core is defined; paths are absolute because
# include() keeps the including directory as the source directory.
target_sources(nevr_core PRIVATE "${CMAKE_SOURCE_DIR}/src/core/map_derive.cpp" "${CMAKE_SOURCE_DIR}/src/core/map_derive.h")
# The core PCH brings windows.h without NOMINMAX; this file is platform-free and uses std::min.
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/core/map_derive.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(BUILD_TESTING)
  find_package(GTest QUIET)
  find_package(zstd CONFIG QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND AND TARGET zstd::libzstd)
  # Memory files (the host tests' fake Env and its SHA-256), a fake StockSource, zstd to read the merged set back, and the
  # SDK's set parser. The shared recipe vectors are read from the source tree.
  add_executable(map_derive_test "${CMAKE_SOURCE_DIR}/src/core/tests/map_derive_test.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/map_derive.cpp" "${CMAKE_SOURCE_DIR}/src/quest/data/sha256.cpp")
  target_include_directories(map_derive_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_compile_definitions(map_derive_test PRIVATE NEVR_RECIPE_VECTORS="${CMAKE_SOURCE_DIR}/tests/vectors/maps/recipes")
  target_link_libraries(map_derive_test PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json
    zstd::libzstd)
  include(GoogleTest)
  gtest_discover_tests(map_derive_test DISCOVERY_MODE PRE_TEST)
endif()
