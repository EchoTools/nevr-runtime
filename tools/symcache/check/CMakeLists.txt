# The compile check of the generated symbol table (src/runtime/log/symcache_data.cpp), which the echo-symbols-sync workflow runs
# on the table it regenerates before it proposes the change:
#
#   cd tools/symcache/check && cmake --preset symcache-check && cmake --build --preset symcache-check
#
# A project of its own, with the host compiler and no dependency: the runtime target compiles this file with the rest of the
# runtime (MinGW and the vcpkg manifest), none of which the workflow's runner has, and the file includes only
# runtime/log/symcache.h. -fsyntax-only writes no object, so ninja runs the check every time it is built.
cmake_minimum_required(VERSION 3.25)
project(nevr_symcache_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(NEVR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(nevr_symcache_data_check OBJECT "${NEVR_REPO_ROOT}/src/runtime/log/symcache_data.cpp")
target_include_directories(nevr_symcache_data_check PRIVATE "${NEVR_REPO_ROOT}/src")
target_compile_options(nevr_symcache_data_check PRIVATE -fsyntax-only)
