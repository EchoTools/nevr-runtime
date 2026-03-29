# GamePatches v2 — frozen backup of the monolithic gamepatches before restructuring.
# Not built by default. Kept for reference only.
# To build: uncomment add_subdirectory(src/legacy/gamepatches-v2) in root CMakeLists.txt

set(PATCHES_V2_SOURCES "dllmain.cpp" "patches.cpp" "plugin_loader.cpp" "winhttp_stub.cpp" "nevr_client.cpp")
set(PATCHES_V2_HEADERS "patches.h" "plugin_loader.h" "winhttp_stub.h" "nevr_client.h" "social_messages.h")

add_library(gamepatchesv2 SHARED ${PATCHES_V2_SOURCES} ${PATCHES_V2_HEADERS} "exports.def")

target_compile_definitions(gamepatchesv2 PRIVATE USE_MINHOOK)

target_precompile_headers(gamepatchesv2 PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")
target_include_directories(gamepatchesv2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(CURL REQUIRED)
target_link_libraries(gamepatchesv2 PRIVATE minhook common version CURL::libcurl)
target_link_directories(gamepatchesv2 PRIVATE common)

set_target_properties(gamepatchesv2 PROPERTIES
  OUTPUT_NAME "gamepatchesv2"
  PREFIX ""
)

if(MSVC)
  target_compile_options(gamepatchesv2 PRIVATE /wd4251)
endif()

if(MINGW)
  target_link_options(gamepatchesv2 PRIVATE
    -Wl,--allow-multiple-definition
  )
endif()
