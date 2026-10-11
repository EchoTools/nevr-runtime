# PUC-Rio Lua 5.4 binding. The VM is built from the extern/lua submodule (pinned to
# the upstream release tag) with our own source list, not Lua's makefile:
#   - lua.c, luac.c (stand-alone programs), ltests.c, onelua.c are not built;
#   - liolib.c, loadlib.c, ldblib.c, linit.c are not built: the sandbox exposes no
#     io, package or debug library, so none of that code is linked at all.
# Lua is compiled as C, so a Lua error is a longjmp (ldo.c LUAI_THROW). The binding
# is written so that no C++ frame with a non-trivial destructor sits between a
# lua_error and the lua_pcall that catches it (see lua54_vm.cpp), which also means
# it does not need C++ exceptions: the binding builds with -fno-exceptions.
set(NEVR_LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../extern/lua")
if(NOT EXISTS "${NEVR_LUA_DIR}/lua.h")
  message(FATAL_ERROR "extern/lua is empty: run `git submodule update --init extern/lua`")
endif()

set(NEVR_LUA_SOURCES
  lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c
  lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c
  lvm.c lzio.c
  lauxlib.c lbaselib.c lcorolib.c lmathlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c)
list(TRANSFORM NEVR_LUA_SOURCES PREPEND "${NEVR_LUA_DIR}/")

add_library(nevr_lua54 STATIC ${NEVR_LUA_SOURCES})
set_target_properties(nevr_lua54 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(nevr_lua54 SYSTEM PUBLIC "${NEVR_LUA_DIR}")
# Lua's own sources: warnings are reported, never errors.
target_compile_options(nevr_lua54 PRIVATE -O2 -Wall -Wextra)
if(ANDROID)
  # Bionic is POSIX: _setjmp/_longjmp, gmtime_r/localtime_r (callbacks run on any game thread).
  target_compile_definitions(nevr_lua54 PRIVATE LUA_USE_POSIX)
endif()

add_library(nevr_script_vm STATIC lua54_vm.cpp)
target_link_libraries(nevr_script_vm PUBLIC nevr_script_host PRIVATE nevr_lua54)
target_compile_options(nevr_script_vm PRIVATE ${NEVR_WARNINGS} -fno-exceptions)

# Extra tests for this binding (cases the shared conformance file does not reach).
nevr_script_test(nevr_lua54_extra_test tests/lua54_extra_test.cpp)
target_link_libraries(nevr_lua54_extra_test PRIVATE nevr_script_vm nevr_script_host)
