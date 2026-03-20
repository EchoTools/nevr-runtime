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

add_library(gamepatcheslegacy SHARED ${PATCHES_LEGACY_SOURCES} ${PATCHES_LEGACY_HEADERS} "exports.def")

target_compile_definitions(gamepatcheslegacy PRIVATE USE_MINHOOK)

target_precompile_headers(gamepatcheslegacy PRIVATE "common/pch.h")
target_include_directories(gamepatcheslegacy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_link_libraries(gamepatcheslegacy PRIVATE minhook)

# Set output name to remove 'lib' prefix on MinGW
set_target_properties(gamepatcheslegacy PROPERTIES
  OUTPUT_NAME "gamepatcheslegacy"
  PREFIX ""
)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(gamepatcheslegacy PRIVATE /wd4251)
endif()
