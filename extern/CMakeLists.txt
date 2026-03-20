# External dependencies - vendored libraries

# MinHook - function hooking library
# Suppress cmake_minimum_required deprecation warning from old version requirement
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
add_subdirectory(minhook)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)
