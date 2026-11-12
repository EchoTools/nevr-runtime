# CDN_DESIGN PC8 (docs/adr/0047): the ConfigAlias lookup detour (runtime/content/config_alias_hook.h) in nevr_runtime,
# and its test. Included through maps/maps.cmake from src/runtime/CMakeLists.txt, after nevr_runtime is defined. The
# pure half (core/config_alias.cpp) is in nevr_core (src/core/maps/pc8core.cmake).
set(_pc8_runtime "${CMAKE_SOURCE_DIR}/src/runtime")

target_sources(nevr_runtime PRIVATE "${_pc8_runtime}/content/config_alias_hook.cpp")

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # The detour against a stub of the game's balance lookup and a fake game context, built without its install half
  # (NEVR_CONFIG_ALIAS_HOOK_TESTING), beside the real CJson_GetFloat detour for the round_time precedence row. Nothing
  # is hooked.
  set(PC8_TEST_SOURCES "${_pc8_runtime}/tests/test_config_alias_hook.cpp" "${_pc8_runtime}/content/config_alias_hook.cpp"
    "${_pc8_runtime}/lifecycle/arena_rules_hook.cpp")
  add_executable(test_config_alias_hook ${PC8_TEST_SOURCES})
  set_source_files_properties(${PC8_TEST_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_compile_definitions(test_config_alias_hook PRIVATE NEVR_CONFIG_ALIAS_HOOK_TESTING)
  target_include_directories(test_config_alias_hook PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_config_alias_hook PRIVATE GTest::gtest GTest::gtest_main nevr_core
    nlohmann_json::nlohmann_json nevr::plugin_sdk)
  include(GoogleTest)
  gtest_discover_tests(test_config_alias_hook DISCOVERY_MODE PRE_TEST)
endif()
