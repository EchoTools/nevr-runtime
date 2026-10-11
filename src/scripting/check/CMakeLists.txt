# nevr_script_check, the author-side type checker (host build; it does not ship in
# the runtime). Uses upstream's own targets for Luau.Analysis from extern/luau.
#
#   cmake -S src/scripting/check -B build/script-check -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/script-check
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
