# CustomAssets DLL - Custom loadout asset loading with HTTP download and caching

# Use vendored MinHook for function hooking
set(HOOKING_LIBRARY minhook::minhook)

set(CUSTOM_ASSETS_SOURCES
    "dllmain.cpp"
    "custom_assets.cpp"
    "config_manager.cpp"
    "cache_manager.cpp"
    "http_client.cpp"
    "asset_converter.cpp"
    "asset_interceptor.cpp"
)

set(CUSTOM_ASSETS_HEADERS
    "custom_assets.h"
    "config_manager.h"
    "cache_manager.h"
    "http_client.h"
    "asset_converter.h"
    "asset_interceptor.h"
)

add_library(CustomAssets SHARED
    ${CUSTOM_ASSETS_SOURCES}
    ${CUSTOM_ASSETS_HEADERS}
    "exports.def"
)

target_compile_definitions(CustomAssets PRIVATE 
    USE_MINHOOK
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(CustomAssets PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    "${CMAKE_SOURCE_DIR}/common"
    "${NEVR_COMMON_INCLUDE}"
)

target_link_libraries(CustomAssets PRIVATE 
    ${HOOKING_LIBRARY}
    Common
    winhttp
    shlwapi
)

target_compile_features(CustomAssets PRIVATE cxx_std_17)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(CustomAssets PRIVATE /wd4251 /wd4267)
endif()
