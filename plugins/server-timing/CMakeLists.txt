add_library(server_timing SHARED
    src/plugin.cpp
)

target_include_directories(server_timing PRIVATE src)
target_compile_definitions(server_timing PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(server_timing PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(server_timing PRIVATE minhook::minhook)

if(WIN32 OR MINGW)
    target_compile_definitions(server_timing PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(server_timing PROPERTIES
    OUTPUT_NAME "server_timing"
    PREFIX ""
)
