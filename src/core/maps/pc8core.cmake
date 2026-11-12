# CDN_DESIGN PC8 (docs/adr/0047): ConfigAlias's pure half (core/config_alias.h): the staged and armed tables, the winner
# rule and the interned snapshots the runtime's lookup detour reads. Included through maps/maps.cmake from
# src/core/CMakeLists.txt, after nevr_core is defined.
target_sources(nevr_core PRIVATE "${CMAKE_SOURCE_DIR}/src/core/config_alias.cpp")

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # The table, the winner rule, the snapshot cap and source B's revalidated set, with Log() captured through the boot
  # sink and operator new counted. No game state.
  set(_pc8core_test "${CMAKE_SOURCE_DIR}/src/core/tests/config_alias_test.cpp")
  add_executable(config_alias_test "${_pc8core_test}")
  set_source_files_properties("${_pc8core_test}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(config_alias_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(config_alias_test PRIVATE GTest::gtest GTest::gtest_main nevr_core)
  include(GoogleTest)
  gtest_discover_tests(config_alias_test DISCOVERY_MODE PRE_TEST)
endif()
