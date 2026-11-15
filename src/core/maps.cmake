# The custom-map channel's core (docs/adr/0047), in nevr_core, and its tests. Included from src/core/CMakeLists.txt
# after nevr_core is defined; each part of the channel adds its core sources and tests here.

# The PE rules a map's script DLL must meet (core/script_image_rules.h; ADR 0047 decision 6). The ELF half
# (script_image_rules_elf.cpp) is the Quest's: its Quest consumer compiles it, and test_script_image_rules below runs it.
if(WIN32)
  target_sources(nevr_core PRIVATE "${CMAKE_CURRENT_LIST_DIR}/script_image_rules.cpp")
endif()

# ConfigAlias's pure half (core/config_alias.h; ADR 0047 decision 5): the staged and armed tables, the winner rule and
# the interned snapshots the runtime's lookup detour reads.
target_sources(nevr_core PRIVATE "${CMAKE_CURRENT_LIST_DIR}/config_alias.cpp")

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  include(GoogleTest)

  # The table, the winner rule, the snapshot cap and the revalidated sublevel set, with Log() captured through the boot
  # sink and operator new counted. No game state.
  add_executable(test_config_alias "${CMAKE_CURRENT_LIST_DIR}/tests/config_alias_test.cpp")
  set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/tests/config_alias_test.cpp"
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_config_alias PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_config_alias PRIVATE GTest::gtest GTest::gtest_main nevr_core)
  gtest_discover_tests(test_config_alias DISCOVERY_MODE PRE_TEST)

  # The PE and the ELF rules, one must-fail case per rule, over images the test builds. Pure: both rule files are
  # compiled into the test, so the ELF half runs on the PC build too.
  set(_maps_rules_test_sources
    "${CMAKE_CURRENT_LIST_DIR}/tests/script_image_rules_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/script_image_rules.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/script_image_rules_elf.cpp")
  add_executable(test_script_image_rules ${_maps_rules_test_sources})
  set_source_files_properties(${_maps_rules_test_sources}
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_script_image_rules PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_script_image_rules PRIVATE GTest::gtest GTest::gtest_main)
  gtest_discover_tests(test_script_image_rules DISCOVERY_MODE PRE_TEST)
endif()
