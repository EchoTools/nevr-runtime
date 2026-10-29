# A project of its own around cmake/libr15_extra_rows.cmake's nevr_libr15_extra_rows, so the host recipe can show that a file the
# patcher would refuse stops a configure, without configuring the whole Android project. It calls the function with the arguments
# src/quest/sentinel/CMakeLists.txt gives it.
cmake_minimum_required(VERSION 3.23)
project(nevr_extra_rows_configure LANGUAGES NONE)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/libr15_extra_rows.cmake")
nevr_libr15_extra_rows("${NEVR_LIBR15_EXTRA_ROWS}" "${CMAKE_CURRENT_BINARY_DIR}/generated.cpp" "${NEVR_SRC_ROOT}" "${NEVR_JSON_INCLUDE}"
                       "${CMAKE_CURRENT_BINARY_DIR}/check")
