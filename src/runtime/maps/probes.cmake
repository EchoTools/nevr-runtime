# The owner probes for the custom-map channel (D:\b\cdnresearch\OWNER_PROBES.md, PB0-PB3). Included through
# maps/maps.cmake, after nevr_runtime is defined. The probes read game memory and log; P-SUBLEVEL adds log-only
# Enable/Disable detours and P-SKEW can leave a match. They are diagnostics for the owner's in-game checks, so they are
# compiled into nevr-runtime.dll only in a probe build, and even there each one stays off until maps_probe.yaml turns
# it on. The game-free half's test builds in every build with tests.
option(NEVR_MAPS_PROBES "Compile the custom-map owner probes into nevr-runtime.dll (probe builds only, never shipped)" OFF)
set(_probes_dir "${CMAKE_SOURCE_DIR}/src/runtime/maps/probe")
set(_probes_logic "${_probes_dir}/probe_logic.cpp")

if(NEVR_MAPS_PROBES)
  set(_probes_sources "${_probes_logic}" "${_probes_dir}/probe_config.cpp" "${_probes_dir}/probe_level_events.cpp"
    "${_probes_dir}/probe_rules.cpp" "${_probes_dir}/probe_sublevel.cpp" "${_probes_dir}/probe_skew.cpp")
  target_sources(nevr_runtime PRIVATE ${_probes_sources})
  set_source_files_properties(${_probes_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_compile_definitions(nevr_runtime PRIVATE NEVR_MAPS_PROBES=1)
  message(STATUS "NEVR_MAPS_PROBES=ON: nevr-runtime.dll carries the custom-map owner probes (probe build, never ship)")
endif()

if(BUILD_TESTING)
  find_package(GTest QUIET)
endif()
if(BUILD_TESTING AND GTest_FOUND)
  set(_probes_test_sources "${CMAKE_SOURCE_DIR}/src/runtime/tests/test_probe_config.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/tests/test_probe_sublevel.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/tests/test_probe_skew.cpp" "${_probes_logic}")
  add_executable(test_maps_probes ${_probes_test_sources})
  set_source_files_properties(${_probes_test_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  target_include_directories(test_maps_probes PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(test_maps_probes PRIVATE GTest::gtest GTest::gtest_main nevr::plugin_sdk)
  include(GoogleTest)
  gtest_discover_tests(test_maps_probes DISCOVERY_MODE PRE_TEST)
endif()
