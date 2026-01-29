# GamePatchesLegacy - self-contained frozen module
# Uses MinHook for function hooking (built from extern/minhook)
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
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_link_libraries(GamePatchesLegacy PRIVATE minhook)
# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(GamePatchesLegacy PRIVATE /wd4251)
endif()
