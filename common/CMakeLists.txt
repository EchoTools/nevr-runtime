# Find Detours

# Game Patches
set(PATCHES_SOURCES "${NEVR_COMMON_INCLUDE}/echovrInternal.hpp" "globals.cpp"
                    "logging.cpp")
set(PATCHES_HEADERS "${NEVR_COMMON_INCLUDE}/echovrInternal.h" "globals.h"
                    "logging.h")

add_library(Common STATIC ${PATCHES_SOURCES} ${PATCHES_HEADERS})

target_precompile_headers(Common PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")

target_include_directories(
  Common PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/common/include
                 ${CMAKE_SOURCE_DIR}/nevr-common.git/include)

target_compile_options(Common PRIVATE /wd4251)
