# Common library for NEVR
set(COMMON_SOURCES "${NEVR_COMMON_INCLUDE}/echovrInternal.cpp" "globals.cpp"
                   "logging.cpp")
set(COMMON_HEADERS
    "${NEVR_COMMON_INCLUDE}/echovr.h" "${NEVR_COMMON_INCLUDE}/echovrInternal.h"
    "globals.h" "logging.h")

add_library(Common STATIC ${COMMON_SOURCES} ${COMMON_HEADERS})

target_precompile_headers(Common PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")

target_include_directories(
  Common PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/common
                 ${CMAKE_SOURCE_DIR}/nevr-common.git/include)

target_compile_options(Common PRIVATE /wd4251)
