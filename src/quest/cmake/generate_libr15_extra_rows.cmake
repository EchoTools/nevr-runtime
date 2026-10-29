# Script entry of cmake/libr15_extra_rows.cmake: the generation alone, no host check.
#   cmake -DNEVR_EXTRA_ROWS_FILE=<file> -DNEVR_EXTRA_ROWS_OUT=<cpp> -P generate_libr15_extra_rows.cmake
include("${CMAKE_CURRENT_LIST_DIR}/libr15_extra_rows.cmake")
if(NOT NEVR_EXTRA_ROWS_FILE OR NOT NEVR_EXTRA_ROWS_OUT)
  message(FATAL_ERROR "usage: cmake -DNEVR_EXTRA_ROWS_FILE=<file> -DNEVR_EXTRA_ROWS_OUT=<cpp> -P generate_libr15_extra_rows.cmake")
endif()
nevr_libr15_generate_extra_rows_source("${NEVR_EXTRA_ROWS_FILE}" "${NEVR_EXTRA_ROWS_OUT}")
