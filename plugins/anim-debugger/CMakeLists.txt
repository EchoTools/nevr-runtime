add_library(anim_debugger SHARED
    src/plugin.cpp
)

target_include_directories(anim_debugger PRIVATE src)
target_compile_definitions(anim_debugger PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(anim_debugger PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(anim_debugger PRIVATE minhook::minhook)

if(WIN32 OR MINGW)
    target_compile_definitions(anim_debugger PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(anim_debugger PROPERTIES
    OUTPUT_NAME "anim_debugger"
    PREFIX ""
)
