# Find Detours
find_library(DETOURS_LIBRARY detours REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")

# Game Patches
set(PATCHES_SOURCES "dllmain.cpp" "patches.cpp")
set(PATCHES_HEADERS "patches.h")

add_library(GamePatches SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS})
target_precompile_headers(GamePatches PRIVATE
                          "${CMAKE_CURRENT_SOURCE_DIR}/../include/pch.h")

target_sources(GamePatches PRIVATE exports.def)
target_include_directories(GamePatches PRIVATE ${DETOURS_INCLUDE_DIRS})
target_link_libraries(GamePatches PRIVATE ${DETOURS_LIBRARY})
target_compile_options(GamePatches PRIVATE /wd4251)
