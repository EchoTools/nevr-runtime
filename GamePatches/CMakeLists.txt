# Find Detours
find_library(DETOURS_LIBRARY detours REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")

# Game Patches
set(PATCHES_SOURCES "dllmain.cpp" "patches.cpp")
set(PATCHES_HEADERS "patches.h")

add_library(GamePatches SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS}
                               "exports.def")

target_precompile_headers(GamePatches PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")
target_include_directories(GamePatches PRIVATE ${DETOURS_INCLUDE_DIRS}
                                               ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(GamePatches PRIVATE ${DETOURS_LIBRARY} Common)
target_link_directories(GamePatches PRIVATE Common)
target_compile_options(GamePatches PRIVATE /wd4251)
