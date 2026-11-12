# CDN_PC PC1 (docs/adr/0047): the PC's cargo Env and map MapFs (runtime/cargo/win_env.h), and their test. Included through
# maps/maps.cmake from src/runtime/CMakeLists.txt, after nevr_runtime is defined; paths are absolute because include() keeps
# the including directory as the source directory.
set(_pc1_runtime "${CMAKE_SOURCE_DIR}/src/runtime")

target_sources(nevr_runtime PRIVATE "${_pc1_runtime}/cargo/win_env.cpp" "${_pc1_runtime}/cargo/win_env.h")
set_source_files_properties("${_pc1_runtime}/cargo/win_env.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_link_libraries(nevr_runtime PRIVATE bcrypt)

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # Win32 files in a temp tree, BCrypt, and libcurl against the scripted loopback server; the cargo store (core/cargo_store.cpp)
  # runs on the env. The test supplies its own Log().
  add_executable(test_win_env "${_pc1_runtime}/tests/test_win_env.cpp" "${_pc1_runtime}/cargo/win_env.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/cargo_store.cpp" "${CMAKE_SOURCE_DIR}/src/core/cargo_manifest.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/cargo_plan.cpp" "${CMAKE_SOURCE_DIR}/src/core/resource_shadow.cpp")
  set_source_files_properties("${_pc1_runtime}/tests/test_win_env.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_win_env PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_win_env PRIVATE GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json CURL::libcurl bcrypt ws2_32)
  include(GoogleTest)
  gtest_discover_tests(test_win_env DISCOVERY_MODE PRE_TEST)
endif()
