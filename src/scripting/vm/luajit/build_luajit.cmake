# Builds libluajit.a with upstream's own Makefile (doc/install.html, "Cross-compiling").
# Run as: cmake -DLJ_SRC=... -DLJ_OUT=... -DLJ_FLAVOUR=mingw|android ... -P build_luajit.cmake
#
# The Makefile writes generated files next to the sources (buildvm, minilua,
# luajit.h, lj_vm.S), so the submodule is copied to LJ_OUT first and the
# submodule tree stays clean. buildvm and minilua run on the build host, so the
# host compiler (LJ_HOST_CC) builds them and the cross compiler builds the rest.
foreach(var LJ_SRC LJ_OUT LJ_FLAVOUR LJ_JOBS)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "build_luajit.cmake: ${var} is not set")
  endif()
endforeach()

file(MAKE_DIRECTORY "${LJ_OUT}")
file(COPY "${LJ_SRC}/" DESTINATION "${LJ_OUT}" PATTERN ".git" EXCLUDE)
# src/Makefile reads ../.relver when there is no .git directory.
if(LJ_RELVER)
  file(WRITE "${LJ_OUT}/.relver" "${LJ_RELVER}\n")
endif()

if(LJ_FLAVOUR STREQUAL "mingw")
  # LJ_CROSS is the compiler prefix, e.g. x86_64-w64-mingw32-
  set(make_args CROSS=${LJ_CROSS} TARGET_SYS=Windows BUILDMODE=static HOST_CC=${LJ_HOST_CC})
elseif(LJ_FLAVOUR STREQUAL "android")
  # The recipe from doc/install.html, "Android/ARM64". LJ_NDK_BIN is the NDK
  # llvm bin directory and LJ_NDK_CC the aarch64-linux-androidNN-clang driver.
  set(make_args
      CROSS=${LJ_NDK_BIN}/aarch64-linux-android-
      TARGET_SYS=Linux BUILDMODE=static HOST_CC=${LJ_HOST_CC}
      STATIC_CC=${LJ_NDK_CC} "DYNAMIC_CC=${LJ_NDK_CC} -fPIC"
      TARGET_LD=${LJ_NDK_CC} "TARGET_AR=${LJ_NDK_BIN}/llvm-ar rcus"
      TARGET_STRIP=${LJ_NDK_BIN}/llvm-strip)
else()
  message(FATAL_ERROR "unknown LJ_FLAVOUR '${LJ_FLAVOUR}'")
endif()
if(LJ_XCFLAGS)
  list(APPEND make_args "XCFLAGS=${LJ_XCFLAGS}")
endif()

execute_process(
  COMMAND make -j${LJ_JOBS} libluajit.a ${make_args}
  WORKING_DIRECTORY "${LJ_OUT}/src"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "LuaJIT make failed with ${rc}")
endif()
file(WRITE "${LJ_OUT}/src/.nevr_built" "ok\n")
