# The q15t plugin's host tests (ADR 0041: native code is built by CMake). Each test is the plugin's own sources built for the host
# against fake game memory and a fake loader, one program per module (mod/<module>.cmake), plus the core programs below.
# A second configure of this same project with the NDK toolchain file (preset q15t-ndk-checks) builds the arm64 compile checks
# and the thunk programs, and ctest judges them with the checks under check/ (they read the built objects).
#   cmake --preset q15t-host-tests && cmake --build --preset q15t-host-tests && ctest --preset q15t-host-tests
# `just test-quest-q15t [module]` runs both configures. The mutation run (mutants.sh) configures this project on a scratch copy.
cmake_minimum_required(VERSION 3.23)
project(q15t_host_tests LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # -std=c++17, as every test was built before
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)  # no source is a module; ccache refuses a compile that carries the scan flags
# The Android toolchain file adds -lm -latomic as standard libraries; pb_thunk_qemu is linked -nostdlib and has no libc to ask them of.
set(CMAKE_CXX_STANDARD_LIBRARIES "")

# Where the plugin's sources and the SDK headers are. The mutation run points the first at a scratch copy of src/.
set(Q15T_PLUGIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../plugins/q15t" CACHE PATH "The q15t plugin directory (src/ holds the sources under test)")
set(Q15T_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../../../sdk/include" CACHE PATH "The plugin SDK headers (nevr/ lives under it)")
get_filename_component(Q15T_PLUGIN_DIR "${Q15T_PLUGIN_DIR}" ABSOLUTE)
get_filename_component(Q15T_SDK_INCLUDE "${Q15T_SDK_INCLUDE}" ABSOLUTE)
set(Q15T_PLUGIN_SRC "${Q15T_PLUGIN_DIR}/src")
set(Q15T_TESTS_DIR "${CMAKE_CURRENT_LIST_DIR}")

set(Q15T_BUILD_ID_FOR_TEST "b243509c08ce677aeb95fa348016949b3fc45230")
# What a host test is built with: the same flags every script line had.
set(Q15T_HOST_OPTIONS -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror)
# What an arm64 check is built with: the plugin's own flags (src/quest/plugins/q15t/CMakeLists.txt) at -O2.
set(Q15T_ARM64_OPTIONS -O2 -fno-exceptions -fno-rtti -fvisibility=hidden -Wall -Wextra -Werror)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(Q15T_ARM64 TRUE)
  get_filename_component(Q15T_NDK_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(tool llvm-readelf llvm-objdump llvm-nm)
    if(NOT EXISTS "${Q15T_NDK_BIN}/${tool}")
      message(FATAL_ERROR "q15t: ${tool} is not beside the NDK clang (${Q15T_NDK_BIN}); Q15T_NDK must be an Android NDK r26d install")
    endif()
  endforeach()
else()
  set(Q15T_ARM64 FALSE)
endif()

# q15t_host_test(<program> TEST <file in this directory> SRC <plugin src names> [options])
# Builds <program> from the test file plus src/<name>.cpp for each SRC name and adds the ctest test <module>.<program>, labelled
# <module> and run with ARGS. The module is the mod/<module>.cmake being included. Options:
#   BASE_OPTS     replaces the default compile options (Q15T_HOST_OPTIONS)
#   OPTS          compile options added after them
#   LINK_OPTS     link options (-rdynamic, -pthread)
#   LIBS          libraries (dl, pthread)
#   DEFS          compile definitions
#   INCLUDES      replaces the default include directories (plugin src, then the SDK)
#   NO_INCLUDES   no include directory at all
#   OBJECTS       object files linked in (generator expressions)
#   DEPENDS       targets that must be built first (a stand-in library the program loads)
#   TIMEOUT       seconds the test may run (the old `timeout N`)
#   ARGS          arguments the program is run with
function(q15t_host_test program)
  if(Q15T_ARM64)
    return()
  endif()
  cmake_parse_arguments(T "NO_INCLUDES" "TEST;TIMEOUT" "SRC;BASE_OPTS;OPTS;LINK_OPTS;LIBS;DEFS;INCLUDES;OBJECTS;DEPENDS;ARGS" ${ARGN})
  set(sources "${Q15T_TESTS_DIR}/${T_TEST}")
  foreach(s IN LISTS T_SRC)
    list(APPEND sources "${Q15T_PLUGIN_SRC}/${s}.cpp")
  endforeach()
  add_executable(${program} ${sources} ${T_OBJECTS})
  if(T_BASE_OPTS)
    set(base ${T_BASE_OPTS})
  else()
    set(base ${Q15T_HOST_OPTIONS})
  endif()
  target_compile_options(${program} PRIVATE ${base} ${T_OPTS})
  if(NOT T_NO_INCLUDES)
    if(T_INCLUDES)
      target_include_directories(${program} PRIVATE ${T_INCLUDES})
    else()
      target_include_directories(${program} PRIVATE "${Q15T_PLUGIN_SRC}" "${Q15T_SDK_INCLUDE}")
    endif()
  endif()
  if(T_DEFS)
    target_compile_definitions(${program} PRIVATE ${T_DEFS})
  endif()
  if(T_LINK_OPTS)
    target_link_options(${program} PRIVATE ${T_LINK_OPTS})
  endif()
  if(T_LIBS)
    target_link_libraries(${program} PRIVATE ${T_LIBS})
  endif()
  if(T_DEPENDS)
    add_dependencies(${program} ${T_DEPENDS})
  endif()
  add_dependencies(mod_${Q15T_MODULE} ${program})
  add_test(NAME ${Q15T_MODULE}.${program} COMMAND ${program} ${T_ARGS})
  set_tests_properties(${Q15T_MODULE}.${program} PROPERTIES LABELS ${Q15T_MODULE})
  if(T_TIMEOUT)
    set_tests_properties(${Q15T_MODULE}.${program} PROPERTIES TIMEOUT ${T_TIMEOUT})
  endif()
endfunction()

# q15t_extra_run(<test name> <program> [args]): one more run of a program q15t_host_test built, with other arguments (ctest name <module>.<test name>).
function(q15t_extra_run name program)
  if(Q15T_ARM64)
    return()
  endif()
  add_test(NAME ${Q15T_MODULE}.${name} COMMAND ${program} ${ARGN})
  set_tests_properties(${Q15T_MODULE}.${name} PROPERTIES LABELS ${Q15T_MODULE})
endfunction()

# q15t_arm64_object(<src name>...): src/<name>.cpp compiled for arm64 with the NDK clang, with the plugin's own warning flags, and
# two tests: the object has no static initializer section (.init_array / .ctors: the repack refuses a plugin with one), and it defines
# no emulated thread-local variable (__emutls_v.*: the API 26 toolchain emulates thread_local and allocates on a thread's first use,
# which a hooked call must not do; the sentinel's TestNoEmulatedTLSInHookPath holds the same line for its own library).
function(q15t_arm64_object)
  if(NOT Q15T_ARM64)
    return()
  endif()
  foreach(m IN LISTS ARGN)
    add_library(nevr_q15t_arm64_${m} OBJECT "${Q15T_PLUGIN_SRC}/${m}.cpp")
    target_compile_options(nevr_q15t_arm64_${m} PRIVATE ${Q15T_ARM64_OPTIONS})
    target_include_directories(nevr_q15t_arm64_${m} PRIVATE "${Q15T_PLUGIN_SRC}" "${Q15T_SDK_INCLUDE}")
    add_dependencies(mod_${Q15T_MODULE} nevr_q15t_arm64_${m})
    add_test(NAME ${Q15T_MODULE}.arm64_${m}
             COMMAND "${Q15T_TESTS_DIR}/check/arm64_no_static_init.sh" "${Q15T_NDK_BIN}/llvm-readelf" "$<TARGET_OBJECTS:nevr_q15t_arm64_${m}>" ${m})
    set_tests_properties(${Q15T_MODULE}.arm64_${m} PROPERTIES LABELS ${Q15T_MODULE})
    add_test(NAME ${Q15T_MODULE}.arm64_no_emutls_${m}
             COMMAND "${Q15T_TESTS_DIR}/check/no_emutls.sh" "${Q15T_NDK_BIN}/llvm-nm" "$<TARGET_OBJECTS:nevr_q15t_arm64_${m}>" ${m})
    set_tests_properties(${Q15T_MODULE}.arm64_no_emutls_${m} PROPERTIES LABELS ${Q15T_MODULE})
  endforeach()
endfunction()

# ---- the core programs: the plugin's entry per arm and its headset gates (label core) ----
set(Q15T_MODULE core)
add_custom_target(mod_core)
foreach(t26 0 1)
  q15t_host_test(host_test_${t26} TEST host_test.cpp
    SRC plugin plugin_log script_redirect import_hook combat_map game_tick safe_read headset_gates got_swap weapon_arm gamespace
    OPTS "SHELL:-include ${Q15T_TESTS_DIR}/host_overrides.h"
    DEFS Q15T_T26=${t26}
         "Q15T_BUILD_ID_FOR_TEST=\"${Q15T_BUILD_ID_FOR_TEST}\""
    LINK_OPTS -rdynamic LIBS dl)
endforeach()
# the headset gates (return-address gated wrappers)
q15t_host_test(gates_test TEST gates_test.cpp SRC headset_gates plugin_log import_hook got_swap LIBS dl)
# the link gate's library name (headset_gates.cpp LibraryName, its dladdr refusal, called directly: the Bind frame is absent on the
# host). The test includes headset_gates.cpp itself, so it is not listed again.
q15t_host_test(library_name_test TEST library_name_test.cpp SRC plugin_log import_hook got_swap LIBS dl)

# ---- one mod/<module>.cmake per module: its host test programs and, in the NDK configure, its arm64 checks ----
file(GLOB Q15T_MODULE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/mod/*.cmake")
foreach(f IN LISTS Q15T_MODULE_FILES)
  get_filename_component(Q15T_MODULE "${f}" NAME_WE)
  add_custom_target(mod_${Q15T_MODULE})
  include("${f}")
endforeach()
