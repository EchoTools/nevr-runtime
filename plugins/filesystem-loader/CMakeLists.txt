add_library(filesystem_loader SHARED
    src/filesystem_loader.cpp
    src/plugin_main.cpp
)

target_include_directories(filesystem_loader PRIVATE src)
target_compile_definitions(filesystem_loader PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(filesystem_loader PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(filesystem_loader PRIVATE minhook::minhook)

if(WIN32 OR MINGW)
    target_compile_definitions(filesystem_loader PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(filesystem_loader PROPERTIES
    OUTPUT_NAME "filesystem_loader"
    PREFIX ""
)
