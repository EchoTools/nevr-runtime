# Common library for NEVR
set(COMMON_SOURCES "echovr_functions.cpp" "globals.cpp"
           "logging.cpp" "base64.cpp")
set(COMMON_HEADERS
  "echovr.h" "echovr_functions.h"
  "globals.h" "logging.h" "base64.h")

add_library(common STATIC ${COMMON_SOURCES} ${COMMON_HEADERS})

target_precompile_headers(common PRIVATE "pch.h")

target_include_directories(
  common PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/common)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(common PRIVATE nlohmann_json::nlohmann_json)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(common PRIVATE /wd4251)
endif()
