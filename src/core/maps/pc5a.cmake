# CDN_DESIGN PC5a (docs/adr/0047): the map channel's core (core/map_run.h, core/map_index.h) in nevr_core, and its test.
# Included through maps/maps.cmake from src/core/CMakeLists.txt, after nevr_core is defined; paths are absolute because
# include() keeps the including directory as the source directory. It uses PC4's parsers (maps/pc4.cmake).
set(_pc5a_core "${CMAKE_SOURCE_DIR}/src/core")

target_sources(nevr_core PRIVATE "${_pc5a_core}/map_run.cpp" "${_pc5a_core}/map_index.cpp")
set_source_files_properties("${_pc5a_core}/map_run.cpp" "${_pc5a_core}/map_index.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_include_directories(nevr_core PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # The boot mount and the fetch over an in-memory Env and MapFs, a scripted server, a recording Mounter and Deriver, and
  # the real PC4 parsers fed manifests the test signs with fixed-seed keys. No network, no Win32, no game file.
  add_executable(map_run_test "${_pc5a_core}/tests/map_run_test.cpp")
  set_source_files_properties("${_pc5a_core}/tests/map_run_test.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(map_run_test PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/sdk/include"
    "${CMAKE_SOURCE_DIR}/third_party/monocypher/src")
  target_link_libraries(map_run_test PRIVATE GTest::gtest GTest::gtest_main nevr_core nlohmann_json::nlohmann_json)
  include(GoogleTest)
  gtest_discover_tests(map_run_test DISCOVERY_MODE PRE_TEST)
endif()
