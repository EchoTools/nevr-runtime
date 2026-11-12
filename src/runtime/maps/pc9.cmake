# CDN_DESIGN PC9 (docs/adr/0047): the session map pin (runtime/content/map_session_pin.h) in nevr_runtime, and its test.
# Included through maps/maps.cmake from src/runtime/CMakeLists.txt, after nevr_runtime and the test targets are defined;
# paths are absolute because include() keeps the including directory as the source directory.
#
# map_session_pin.cpp is the state and the decisions (std, nlohmann and the SDK's symbol hash); map_session_pin_game.cpp
# is the game half (the owner-JSON calls, the error block, the return to lobby) and goes only into nevr_runtime.
set(_pc9_runtime "${CMAKE_SOURCE_DIR}/src/runtime")

target_sources(nevr_runtime PRIVATE "${_pc9_runtime}/content/map_session_pin.cpp"
  "${_pc9_runtime}/content/map_session_pin_game.cpp")
set_source_files_properties("${_pc9_runtime}/content/map_session_pin.cpp"
  "${_pc9_runtime}/content/map_session_pin_game.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# ws_bridge.cpp fills the login's nevr_maps from LoginEntriesJson(); test_behavioral compiles ws_bridge.cpp.
if(TARGET test_behavioral)
  target_sources(test_behavioral PRIVATE "${_pc9_runtime}/content/map_session_pin.cpp")
endif()

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # The pin's state machine over a fake game (map_session_pin_seam.h); the test supplies its own Log(). The login
  # profile is the real one, for the byte-identical check.
  set(PC9_TEST_SOURCES "${_pc9_runtime}/tests/test_map_session_pin.cpp" "${_pc9_runtime}/content/map_session_pin.cpp"
    "${_pc9_runtime}/compat/login_profile.cpp")
  add_executable(test_map_session_pin ${PC9_TEST_SOURCES})
  set_source_files_properties(${PC9_TEST_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_map_session_pin PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_map_session_pin PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main
    nlohmann_json::nlohmann_json)
  include(GoogleTest)
  gtest_discover_tests(test_map_session_pin DISCOVERY_MODE PRE_TEST)
endif()
