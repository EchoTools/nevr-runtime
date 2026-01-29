# GamePatchesLegacy - self-contained frozen module
# Find Detours
find_library(DETOURS_LIBRARY detours REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")

set(PATCHES_LEGACY_SOURCES
    "dllmain.cpp"
    "patches.cpp"
    "common/echovr_internal.cpp"
)
set(PATCHES_LEGACY_HEADERS
    "patches.h"
    "processmem.h"
    "common/pch.h"
    "common/echovr.h"
    "common/echovr_internal.h"
    "common/echovr_unexported.h"
)

add_library(GamePatchesLegacy SHARED ${PATCHES_LEGACY_SOURCES} ${PATCHES_LEGACY_HEADERS} "exports.def")

target_precompile_headers(GamePatchesLegacy PRIVATE "common/pch.h")
target_include_directories(GamePatchesLegacy PRIVATE
    ${DETOURS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_link_libraries(GamePatchesLegacy PRIVATE ${DETOURS_LIBRARY})
target_compile_options(GamePatchesLegacy PRIVATE /wd4251)
