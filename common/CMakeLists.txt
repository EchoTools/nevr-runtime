# Find Detours

# Game Patches
set(PATCHES_SOURCES "echovrInternal.cpp" "globals.cpp" "logging.cpp")
set(PATCHES_HEADERS "echovrInternal.h" "globals.h" "logging.h")

add_library(Common STATIC ${PATCHES_SOURCES} ${PATCHES_HEADERS})

target_precompile_headers(Common PRIVATE "${CMAKE_SOURCE_DIR}/common/pch.h")

target_include_directories(Common PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(Common PRIVATE /wd4251)
