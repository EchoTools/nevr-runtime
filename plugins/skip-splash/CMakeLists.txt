add_library(skip_splash SHARED
    src/plugin.cpp
)

target_include_directories(skip_splash PRIVATE src)
target_compile_definitions(skip_splash PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(skip_splash PRIVATE nevr_plugin_common)

if(WIN32 OR MINGW)
    target_compile_definitions(skip_splash PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(skip_splash PROPERTIES
    OUTPUT_NAME "skip_splash"
    PREFIX ""
)
