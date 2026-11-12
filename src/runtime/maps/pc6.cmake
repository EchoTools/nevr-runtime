# CDN_PC PC6 (docs/adr/0047): the map script provider (runtime/content/script_provider.h) in nevr_runtime, and its test.
# Included through maps/maps.cmake from src/runtime/CMakeLists.txt, after nevr_runtime is defined; paths are absolute
# because include() keeps the including directory as the source directory. The PE rules it applies
# (core/script_image_rules.cpp) are in nevr_core (src/core/maps/pc6core.cmake).
set(_pc6_runtime "${CMAKE_SOURCE_DIR}/src/runtime")

target_sources(nevr_runtime PRIVATE "${_pc6_runtime}/content/script_provider.cpp"
  "${_pc6_runtime}/content/map_scripts_install.cpp")

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # The provider and the PE rules over synthetic DLL images the test builds in a temporary folder; its install half
  # (content/map_scripts_install.cpp) beside a real content pack and the real load rewriters. PC3's SetScriptWaiver is
  # the test's own recorder. Links the real Log(). No game file is read.
  set(PC6_TEST_SOURCES "${_pc6_runtime}/tests/test_map_scripts.cpp" "${_pc6_runtime}/content/script_provider.cpp"
    "${_pc6_runtime}/content/map_scripts_install.cpp" "${_pc6_runtime}/host/file_digest.cpp"
    "${_pc6_runtime}/content/content_json.cpp" "${_pc6_runtime}/content/content_pack.cpp"
    "${_pc6_runtime}/hook/dll_load_hook.cpp" "${_pc6_runtime}/hook/hook_guard.cpp"
    "${_pc6_runtime}/log/boot_log_tee.cpp")
  add_executable(test_map_scripts ${PC6_TEST_SOURCES})
  set_source_files_properties(${PC6_TEST_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_compile_definitions(test_map_scripts PRIVATE NEVR_USE_MINHOOK)
  target_include_directories(test_map_scripts PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_map_scripts PRIVATE GTest::gtest GTest::gtest_main minhook nevr_core
    nlohmann_json::nlohmann_json)
  include(GoogleTest)
  gtest_discover_tests(test_map_scripts DISCOVERY_MODE PRE_TEST)
endif()
