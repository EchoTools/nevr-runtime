# Script host spike: the host API registry, one script VM binding and their tests,
# built on their own with the same toolchains as the real targets:
#
#   Windows:  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake (run under wine)
#   Quest:    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
#             -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static
#             (static executables, run under qemu-aarch64)
#
# NEVR_SCRIPT_VM picks the binding: `null` (no VM; the size baseline) or a
# directory under vm/ whose CMakeLists.txt defines the target nevr_script_vm.
cmake_minimum_required(VERSION 3.22)
project(nevr_scripting LANGUAGES C CXX)

set(NEVR_SCRIPT_VM "null" CACHE STRING "Script VM binding: null or a directory under vm/")
set(NEVR_JSON_INCLUDE_DIR "" CACHE PATH "Directory that holds nlohmann/json.hpp")
if(NOT EXISTS "${NEVR_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
  message(FATAL_ERROR "NEVR_JSON_INCLUDE_DIR must hold nlohmann/json.hpp (got '${NEVR_JSON_INCLUDE_DIR}')")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(NEVR_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(NEVR_WARNINGS -Wall -Wextra -Werror)

find_package(Threads REQUIRED)

add_library(nevr_script_host STATIC host_registry.cpp script_manifest.cpp script_host.cpp script_stubs.cpp)
target_include_directories(nevr_script_host PUBLIC "${NEVR_SRC_DIR}")
target_include_directories(nevr_script_host SYSTEM PUBLIC "${NEVR_JSON_INCLUDE_DIR}")
target_compile_options(nevr_script_host PRIVATE ${NEVR_WARNINGS})

# Test executables are fully static, so wine needs no mingw DLLs beside them and
# qemu-aarch64 needs no Bionic linker.
function(nevr_script_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${NEVR_SRC_DIR}")
  target_include_directories(${name} SYSTEM PRIVATE "${NEVR_JSON_INCLUDE_DIR}")
  target_compile_options(${name} PRIVATE ${NEVR_WARNINGS})
  target_link_libraries(${name} PRIVATE Threads::Threads)
  target_link_options(${name} PRIVATE -static)
endfunction()

if(NEVR_SCRIPT_VM STREQUAL "null")
  add_library(nevr_script_vm STATIC vm/null/null_vm.cpp)
  target_link_libraries(nevr_script_vm PUBLIC nevr_script_host)
  target_compile_options(nevr_script_vm PRIVATE ${NEVR_WARNINGS})
else()
  add_subdirectory("vm/${NEVR_SCRIPT_VM}")
endif()


nevr_script_test(nevr_script_registry_test tests/host_registry_test.cpp)
target_link_libraries(nevr_script_registry_test PRIVATE nevr_script_host)

nevr_script_test(nevr_script_host_test tests/script_host_test.cpp)
target_link_libraries(nevr_script_host_test PRIVATE nevr_script_host)

nevr_script_test(nevr_script_stubs_test tests/script_stubs_test.cpp)
target_link_libraries(nevr_script_stubs_test PRIVATE nevr_script_host)

nevr_script_test(nevr_script_conformance tests/conformance_test.cpp)
target_link_libraries(nevr_script_conformance PRIVATE nevr_script_vm nevr_script_host)
