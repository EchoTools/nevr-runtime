# CDN_DESIGN PC5b (docs/adr/0047): the custom-map channel on the PC (runtime/content/map_channel.h) in nevr_runtime, and its
# test. Included through maps/maps.cmake from src/runtime/CMakeLists.txt, after nevr_runtime is defined; paths are absolute
# because include() keeps the including directory as the source directory.
set(_pc5_runtime "${CMAKE_SOURCE_DIR}/src/runtime")
set(PC5_CHANNEL_SOURCES "${_pc5_runtime}/content/map_channel_core.cpp" "${_pc5_runtime}/content/map_channel_local.cpp")

target_sources(nevr_runtime PRIVATE "${_pc5_runtime}/content/map_channel.cpp" ${PC5_CHANNEL_SOURCES})
set_source_files_properties("${_pc5_runtime}/content/map_channel.cpp" ${PC5_CHANNEL_SOURCES}
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # The channel end to end with no game and no game file: a bucket published into a temp folder and served on loopback,
  # the real Win32 env, cache and libcurl, the real map script provider and config alias table, a fake resource-set
  # registry, and a LOCAL .nevrmap signed with a test dev key.
  set(PC5_TEST_SUPPORT ${PC5_CHANNEL_SOURCES}
    "${_pc5_runtime}/cargo/win_env.cpp" "${_pc5_runtime}/content/script_provider.cpp"
    "${_pc5_runtime}/content/map_session_pin.cpp" "${_pc5_runtime}/compat/login_profile.cpp"
    "${_pc5_runtime}/host/file_digest.cpp" "${_pc5_runtime}/content/content_json.cpp"
    "${_pc5_runtime}/content/content_pack.cpp" "${_pc5_runtime}/hook/dll_load_hook.cpp"
    "${_pc5_runtime}/hook/hook_guard.cpp" "${_pc5_runtime}/log/boot_log_tee.cpp")
  add_executable(test_map_channel "${_pc5_runtime}/tests/test_map_channel.cpp" ${PC5_TEST_SUPPORT})
  # CDN_DESIGN 11's IC checks over EchoConstruct's real output, which the caller stages in <exe dir>\ic. Not discovered
  # and not named test_*: without that folder every one of its tests fails.
  add_executable(ic_map_channel "${_pc5_runtime}/tests/ic_map_channel.cpp" ${PC5_TEST_SUPPORT})
  set_source_files_properties("${_pc5_runtime}/tests/test_map_channel.cpp" "${_pc5_runtime}/tests/ic_map_channel.cpp"
    ${PC5_TEST_SUPPORT} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  foreach(_pc5_test test_map_channel ic_map_channel)
    target_compile_definitions(${_pc5_test} PRIVATE NEVR_USE_MINHOOK
      NEVR_MAPS_CONSTANTS="${CMAKE_SOURCE_DIR}/tests/vectors/maps/constants.json")
    target_include_directories(${_pc5_test} PRIVATE "${CMAKE_SOURCE_DIR}/src"
      "${CMAKE_SOURCE_DIR}/third_party/monocypher/src")
    target_link_libraries(${_pc5_test} PRIVATE GTest::gtest GTest::gtest_main minhook nevr_core nevr::plugin_sdk
      nlohmann_json::nlohmann_json CURL::libcurl ZLIB::ZLIB bcrypt ws2_32)
  endforeach()
  include(GoogleTest)
  gtest_discover_tests(test_map_channel DISCOVERY_MODE PRE_TEST)
endif()
