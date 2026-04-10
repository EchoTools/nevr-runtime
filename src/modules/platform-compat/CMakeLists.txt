# platform-compat module — TLS modernization, CreateDirectory fixes, WinHTTP bridge hook.
# Includes the winhttp_stub (libcurl-backed COM stub) since the CoCreateInstance hook
# redirects WinHTTP COM creation to it.

find_package(CURL REQUIRED)

add_library(platform_compat SHARED
    src/platform_compat.cpp
    "${CMAKE_SOURCE_DIR}/src/gamepatches/winhttp_stub.cpp"
)

target_compile_definitions(platform_compat PRIVATE USE_MINHOOK NEVR_MODULE_EXPORTS)

target_include_directories(platform_compat PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/gamepatches
)

target_precompile_headers(platform_compat PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")

target_link_libraries(platform_compat PRIVATE minhook common CURL::libcurl)
target_link_directories(platform_compat PRIVATE common)

set_target_properties(platform_compat PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules"
)
