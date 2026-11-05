# test-quest-autopilot: the Quest autopilot plugin on the host (#1202, docs/adr/0025-quest-autopilot-route.md): the route grammar,
# every derived bit and limit, the clock, the filter against a fake clock, the event ring (two producers), and the plugin's exports
# against a fake host with a real route file and its real reporter thread; and every shipped route file
# (tools/quest_autopilot_routes/) parses. The start-file case and the two stopped-run cases each run in their own process (the
# plugin keeps one run per process). Built as the device builds it, without exceptions.

nevr_ht_group(autopilot)
set(_ap HOST NOEXC PTHREAD)
set(_ap_scratch "${CMAKE_BINARY_DIR}/scratch")
file(GLOB _routes RELATIVE "${NEVR_REPO_ROOT}" "${NEVR_REPO_ROOT}/tools/quest_autopilot_routes/*.txt")
if(_routes)
  nevr_ht_test(autopilot_route_test GROUP autopilot ${_ap} TIMEOUT 60 ARGS ${_routes}
    SRC src/quest/tests/autopilot_route_test.cpp src/quest/plugins/autopilot/route.cpp)
else()
  nevr_ht_test(autopilot_route_test GROUP autopilot ${_ap} NO_TEST
    SRC src/quest/tests/autopilot_route_test.cpp src/quest/plugins/autopilot/route.cpp)
  message(STATUS "quest host tests: tools/quest_autopilot_routes has no route file; autopilot_route_test is not registered")
endif()
add_test(NAME autopilot_scratch COMMAND ${CMAKE_COMMAND} -E make_directory "${_ap_scratch}")
set_tests_properties(autopilot_scratch PROPERTIES FIXTURES_SETUP autopilot_scratch LABELS autopilot)
nevr_ht_test(autopilot_run_test GROUP autopilot ${_ap} TIMEOUT 120 FIXTURE_REQUIRED autopilot_scratch RESOURCE_LOCK autopilot_scratch
  DEFS "NEVR_AUTOPILOT_BUILD_IDS=\"0000000000000000000000000000000000000000\"" ARGS "${_ap_scratch}"
  SRC src/quest/tests/autopilot_run_test.cpp src/quest/plugins/autopilot/route.cpp src/quest/plugins/autopilot/autopilot.cpp
      src/quest/plugins/autopilot/plugin.cpp src/quest/data/sha256.cpp)
foreach(case start_file stopped stopped_waiting fd_zero)
  nevr_ht_rerun(autopilot_run_test_${case} autopilot_run_test GROUP autopilot TIMEOUT 60 FIXTURE_REQUIRED autopilot_scratch
    RESOURCE_LOCK autopilot_scratch ARGS "${_ap_scratch}" ${case})
endforeach()
