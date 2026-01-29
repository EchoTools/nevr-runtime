# Game Patches uses MinHook for function hooking
# MinHook is built from extern/minhook
set(PATCHES_SOURCES "dllmain.cpp" "patches.cpp")
set(PATCHES_HEADERS "patches.h")

add_library(GamePatches SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS}
                               "exports.def")

target_precompile_headers(GamePatches PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")
target_include_directories(GamePatches PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(GamePatches PRIVATE minhook Common)
target_link_directories(GamePatches PRIVATE Common)
target_compile_options(GamePatches PRIVATE /wd4251)
