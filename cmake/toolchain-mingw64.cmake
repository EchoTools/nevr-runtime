# MinGW-w64 Cross-Compilation Toolchain for Linux
# Produces Windows x64 PE binaries using MinGW-w64

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Find MinGW-w64 compiler
find_program(MINGW_CXX_COMPILER x86_64-w64-mingw32-g++)
find_program(MINGW_C_COMPILER x86_64-w64-mingw32-gcc)
find_program(MINGW_RC_COMPILER x86_64-w64-mingw32-windres)
find_program(MINGW_AR x86_64-w64-mingw32-ar)
find_program(MINGW_RANLIB x86_64-w64-mingw32-ranlib)

if(NOT MINGW_CXX_COMPILER)
    message(FATAL_ERROR "MinGW-w64 not found. Install with: sudo pacman -S mingw-w64-gcc (Arch) or apt install g++-mingw-w64-x86-64 (Debian/Ubuntu)")
endif()

set(CMAKE_C_COMPILER ${MINGW_C_COMPILER})
set(CMAKE_CXX_COMPILER ${MINGW_CXX_COMPILER})
set(CMAKE_RC_COMPILER ${MINGW_RC_COMPILER})
set(CMAKE_AR ${MINGW_AR})
set(CMAKE_RANLIB ${MINGW_RANLIB})

# Target environment
# CRITICAL: When vcpkg chainloads this toolchain, we must APPEND to CMAKE_FIND_ROOT_PATH
# not SET it, otherwise vcpkg's paths get overwritten and find_package fails
list(APPEND CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# MinGW-specific flags
set(CMAKE_CXX_FLAGS_INIT "-I/usr/x86_64-w64-mingw32/include -static-libgcc -static-libstdc++")
set(CMAKE_C_FLAGS_INIT "-I/usr/x86_64-w64-mingw32/include -static-libgcc")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Windows version target (Windows 10)
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
