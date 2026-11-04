# nevr_script_check, the author-side type checker (host build; it does not ship in
# the runtime). Uses upstream's own targets for Luau.Analysis from extern/luau.
# nevr_script_stubs_host_test is the host build of the stubs generator: check_test.sh runs it with --emit to
# write the definitions the checker reads.
#
#   cd src/scripting/check && cmake --preset script-check -DNEVR_JSON_INCLUDE_DIR=<dir that holds nlohmann/json.hpp>
#   cmake --build --preset script-check
cmake_minimum_required(VERSION 3.25)
project(nevr_script_check LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
get_filename_component(NEVR_LUAU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/luau" ABSOLUTE)
add_subdirectory("${NEVR_LUAU_DIR}" luau EXCLUDE_FROM_ALL SYSTEM)  # SYSTEM: upstream headers are not held to our -Werror

add_executable(nevr_script_check nevr_script_check.cpp)
target_link_libraries(nevr_script_check PRIVATE Luau.Analysis)
target_compile_options(nevr_script_check PRIVATE -Wall -Wextra -Werror)

set(NEVR_JSON_INCLUDE_DIR "" CACHE PATH "Directory that holds nlohmann/json.hpp")
if(NOT EXISTS "${NEVR_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
  message(FATAL_ERROR "NEVR_JSON_INCLUDE_DIR must hold nlohmann/json.hpp (got '${NEVR_JSON_INCLUDE_DIR}')")
endif()
find_package(Threads REQUIRED)
get_filename_component(NEVR_SCRIPTING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(NEVR_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(NEVR_SDK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/include" ABSOLUTE)
add_executable(nevr_script_stubs_host_test
  "${NEVR_SCRIPTING_DIR}/host_registry.cpp"
  "${NEVR_SCRIPTING_DIR}/script_stubs.cpp"
  "${NEVR_SCRIPTING_DIR}/tests/script_stubs_test.cpp")
target_include_directories(nevr_script_stubs_host_test PRIVATE "${NEVR_SRC_DIR}" "${NEVR_SDK_INCLUDE_DIR}")
target_include_directories(nevr_script_stubs_host_test SYSTEM PRIVATE "${NEVR_JSON_INCLUDE_DIR}")
target_compile_options(nevr_script_stubs_host_test PRIVATE -O1 -Wall -Wextra -Werror)
target_link_libraries(nevr_script_stubs_host_test PRIVATE Threads::Threads)
