# Find Detours
find_library(DETOURS_LIBRARY detours REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")

# Game Patches Legacy
set(PATCHES_LEGACY_SOURCES "dllmain.cpp" "patches.cpp")
set(PATCHES_LEGACY_HEADERS "patches.h")

add_library(GamePatchesLegacy SHARED ${PATCHES_LEGACY_SOURCES} ${PATCHES_LEGACY_HEADERS}
                               "exports.def")

target_precompile_headers(GamePatchesLegacy PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")
target_include_directories(GamePatchesLegacy PRIVATE ${DETOURS_INCLUDE_DIRS}
                                               ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(GamePatchesLegacy PRIVATE ${DETOURS_LIBRARY} Common)
target_link_directories(GamePatchesLegacy PRIVATE Common)
target_compile_options(GamePatchesLegacy PRIVATE /wd4251)
