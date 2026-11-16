# The test shelf as CMake sees it (tools/test_shelf.json, the one file every runner of the repo reads).
#
#   NEVR_TEST_CEILING_S     the longest a ctest test may run, in seconds: every test gets it as its TIMEOUT
#   NEVR_SHELVED_CTESTS     the ctest tests the shelf names (ids `ctest:<name>`): each is registered DISABLED, so ctest
#                           reports it "Not Run (Disabled)" and a run neither runs nor fails it
#
# Set NEVR_TEST_SHELF to read another file (a test of this module does); the default is tools/test_shelf.json beside cmake/.
if(NOT NEVR_TEST_SHELF)
  set(NEVR_TEST_SHELF "${CMAKE_CURRENT_LIST_DIR}/../tools/test_shelf.json")
endif()
get_filename_component(NEVR_TEST_SHELF "${NEVR_TEST_SHELF}" ABSOLUTE)
if(NOT EXISTS "${NEVR_TEST_SHELF}")
  message(FATAL_ERROR "the test shelf ${NEVR_TEST_SHELF} does not exist: it holds the ceiling every test is held to")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NEVR_TEST_SHELF}")
file(READ "${NEVR_TEST_SHELF}" _nevr_shelf_json)
string(JSON NEVR_TEST_CEILING_S GET "${_nevr_shelf_json}" ceiling_s)
if(NOT NEVR_TEST_CEILING_S MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "${NEVR_TEST_SHELF}: ceiling_s '${NEVR_TEST_CEILING_S}' is not a whole number of seconds")
endif()
set(NEVR_SHELVED_CTESTS "")
string(JSON _nevr_shelf_count LENGTH "${_nevr_shelf_json}" entries)
if(_nevr_shelf_count GREATER 0)
  math(EXPR _nevr_shelf_last "${_nevr_shelf_count} - 1")
  foreach(_nevr_i RANGE ${_nevr_shelf_last})
    string(JSON _nevr_id GET "${_nevr_shelf_json}" entries ${_nevr_i} id)
    if(_nevr_id MATCHES "^ctest:(.+)$")
      list(APPEND NEVR_SHELVED_CTESTS "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()
