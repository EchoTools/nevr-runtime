# LuaJIT 2.1 binding (extern/luajit, pinned submodule). Defines nevr_script_vm.
#
# libluajit.a comes from upstream's Makefile, driven by build_luajit.cmake in a
# copy of the submodule under this build directory (the submodule stays clean).
#
#   NEVR_LUAJIT_JIT_OFF   luaJIT_setmode(..., LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF) in every state
#   NEVR_LUAJIT_CHECKHOOK build LuaJIT with -DLUAJIT_ENABLE_CHECKHOOK, so compiled traces notice a count hook
#   NEVR_LUAJIT_WATCHDOG  experiment: no permanent count hook; a watchdog thread installs one at
#                         the deadline. Builds LuaJIT with -DLUAJIT_ENABLE_CHECKHOOK.
#   NEVR_LUAJIT_BUILD_DIR where libluajit.a is built (share it between configurations)
option(NEVR_LUAJIT_JIT_OFF "Turn the LuaJIT compiler off in every state" OFF)
option(NEVR_LUAJIT_CHECKHOOK "Build LuaJIT with LUAJIT_ENABLE_CHECKHOOK" OFF)
option(NEVR_LUAJIT_WATCHDOG "Enforce the budget with an asynchronous watchdog instead of a count hook" OFF)
set(NEVR_LUAJIT_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/luajit" CACHE PATH "Where libluajit.a is built")

set(LJ_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../extern/luajit")
get_filename_component(LJ_SRC "${LJ_SRC}" ABSOLUTE)
if(NOT EXISTS "${LJ_SRC}/src/lj_obj.h")
  message(FATAL_ERROR "extern/luajit is empty; run: git submodule update --init extern/luajit")
endif()

find_program(LJ_HOST_CC NAMES gcc cc REQUIRED)
execute_process(COMMAND git -C "${LJ_SRC}" show -s --format=%ct HEAD
                OUTPUT_VARIABLE LJ_RELVER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

set(LJ_XCFLAGS "")
if(NEVR_LUAJIT_WATCHDOG OR NEVR_LUAJIT_CHECKHOOK)
  string(APPEND LJ_XCFLAGS " -DLUAJIT_ENABLE_CHECKHOOK")
endif()
string(STRIP "${LJ_XCFLAGS}" LJ_XCFLAGS)

set(LJ_ARGS -DLJ_SRC=${LJ_SRC} -DLJ_OUT=${NEVR_LUAJIT_BUILD_DIR} -DLJ_JOBS=6
            -DLJ_HOST_CC=${LJ_HOST_CC} -DLJ_RELVER=${LJ_RELVER} "-DLJ_XCFLAGS=${LJ_XCFLAGS}")
if(ANDROID)
  get_filename_component(LJ_NDK_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
  string(REGEX REPLACE "^android-" "" LJ_API "${ANDROID_PLATFORM}")
  set(LJ_NDK_CC "${LJ_NDK_BIN}/aarch64-linux-android${LJ_API}-clang")
  list(APPEND LJ_ARGS -DLJ_FLAVOUR=android -DLJ_NDK_BIN=${LJ_NDK_BIN} -DLJ_NDK_CC=${LJ_NDK_CC})
elseif(MINGW OR WIN32)
  # CMAKE_C_COMPILER is <prefix>gcc; upstream wants the prefix (CROSS=).
  get_filename_component(LJ_CC_NAME "${CMAKE_C_COMPILER}" NAME_WE)
  string(REGEX REPLACE "gcc$" "" LJ_CROSS "${LJ_CC_NAME}")
  list(APPEND LJ_ARGS -DLJ_FLAVOUR=mingw -DLJ_CROSS=${LJ_CROSS})
else()
  message(FATAL_ERROR "the LuaJIT binding builds for mingw-w64 and Android arm64 only")
endif()

file(GLOB LJ_SOURCES "${LJ_SRC}/src/*.c" "${LJ_SRC}/src/*.h" "${LJ_SRC}/src/*.dasc" "${LJ_SRC}/src/Makefile")
set(LJ_LIB "${NEVR_LUAJIT_BUILD_DIR}/src/libluajit.a")
add_custom_command(
  OUTPUT "${LJ_LIB}"
  COMMAND ${CMAKE_COMMAND} ${LJ_ARGS} -P "${CMAKE_CURRENT_SOURCE_DIR}/build_luajit.cmake"
  DEPENDS ${LJ_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/build_luajit.cmake"
  COMMENT "Building LuaJIT with its own Makefile"
  VERBATIM)
add_custom_target(luajit_lib DEPENDS "${LJ_LIB}")

add_library(luajit_imported STATIC IMPORTED GLOBAL)
set_target_properties(luajit_imported PROPERTIES IMPORTED_LOCATION "${LJ_LIB}")
if(ANDROID)
  # lj_clib.c (FFI library loading) and lib_package.c call dlopen/dlsym; the scripts never reach them.
  set_target_properties(luajit_imported PROPERTIES INTERFACE_LINK_LIBRARIES dl)
endif()
add_dependencies(luajit_imported luajit_lib)

add_library(nevr_script_vm STATIC luajit_vm.cpp)
target_include_directories(nevr_script_vm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# The generated luajit.h lives in the build copy; SYSTEM keeps upstream headers out of -Werror.
target_include_directories(nevr_script_vm SYSTEM PRIVATE "${NEVR_LUAJIT_BUILD_DIR}/src")
target_compile_options(nevr_script_vm PRIVATE ${NEVR_WARNINGS})
target_link_libraries(nevr_script_vm PUBLIC nevr_script_host luajit_imported)
if(NEVR_LUAJIT_JIT_OFF)
  target_compile_definitions(nevr_script_vm PRIVATE NEVR_LUAJIT_JIT_OFF=1)
endif()
if(NEVR_LUAJIT_WATCHDOG)
  target_compile_definitions(nevr_script_vm PRIVATE NEVR_LUAJIT_WATCHDOG=1)
endif()

# Host-side probe: is the JIT compiling in the binding's own state? Not part of the contract.
add_executable(nevr_luajit_probe probe.cpp)
target_include_directories(nevr_luajit_probe PRIVATE "${NEVR_SRC_DIR}")
target_compile_options(nevr_luajit_probe PRIVATE ${NEVR_WARNINGS})
target_link_libraries(nevr_luajit_probe PRIVATE nevr_script_vm Threads::Threads)
target_link_options(nevr_luajit_probe PRIVATE -static)
