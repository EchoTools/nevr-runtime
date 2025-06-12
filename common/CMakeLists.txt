# Find Detours
find_library(DETOURS_LIBRARY detours REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")

# Game Patches
set(PATCHES_SOURCES "echovrInternal.cpp" "globals.cpp" "logging.cpp")
set(PATCHES_HEADERS "echovrInternal.h" "globals.h" "logging.h")

add_library(Common STATIC ${PATCHES_SOURCES} ${PATCHES_HEADERS})

target_precompile_headers(Common PRIVATE "${CMAKE_SOURCE_DIR}/common/pch.h")

target_include_directories(Common PRIVATE ${DETOURS_INCLUDE_DIRS}
                                          ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Common PRIVATE ${DETOURS_LIBRARY})

target_compile_options(Common PRIVATE /wd4251)
