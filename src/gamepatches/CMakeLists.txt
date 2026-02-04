# Game Patches uses MinHook for function hooking
# MinHook is built from extern/minhook
set(PATCHES_SOURCES "dllmain.cpp" "patches.cpp")
set(PATCHES_HEADERS "patches.h")

add_library(gamepatches SHARED ${PATCHES_SOURCES} ${PATCHES_HEADERS}
                               "exports.def")

target_compile_definitions(gamepatches PRIVATE USE_MINHOOK)

target_precompile_headers(gamepatches PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")
target_include_directories(gamepatches PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(gamepatches PRIVATE minhook common version)
target_link_directories(gamepatches PRIVATE common)

# Set output name to remove 'lib' prefix on MinGW
set_target_properties(gamepatches PROPERTIES
  OUTPUT_NAME "gamepatches"
  PREFIX ""
)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(gamepatches PRIVATE /wd4251)
endif()
