# CDN_DESIGN PC10 (docs/adr/0047): the PC's StockSource over the install (runtime/content/stock_source_win.h), and its test.
# Included through maps/maps.cmake from src/runtime/CMakeLists.txt, after nevr_runtime is defined; paths are absolute because
# include() keeps the including directory as the source directory.
set(_pc10_runtime "${CMAKE_SOURCE_DIR}/src/runtime")

target_sources(nevr_runtime PRIVATE "${_pc10_runtime}/content/stock_source_win.cpp" "${_pc10_runtime}/content/stock_source_win.h")
set_source_files_properties("${_pc10_runtime}/content/stock_source_win.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  # A synthetic install in a temp folder (manifests and packages written by the test, never game files); std, Win32, zstd and
  # the manifest finder (server/installed_levels.cpp) only.
  add_executable(test_stock_source "${_pc10_runtime}/tests/test_stock_source.cpp" "${_pc10_runtime}/content/stock_source_win.cpp"
    "${_pc10_runtime}/server/installed_levels.cpp")
  set_source_files_properties("${_pc10_runtime}/tests/test_stock_source.cpp" "${_pc10_runtime}/server/installed_levels.cpp"
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_stock_source PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_stock_source PRIVATE nevr::plugin_sdk GTest::gtest GTest::gtest_main zstd::libzstd)
  include(GoogleTest)
  gtest_discover_tests(test_stock_source DISCOVERY_MODE PRE_TEST)
endif()
