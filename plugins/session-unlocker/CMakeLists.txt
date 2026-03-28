add_library(session_unlocker SHARED src/plugin.cpp)

target_include_directories(session_unlocker PRIVATE src)
target_compile_definitions(session_unlocker PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(session_unlocker PRIVATE nevr_plugin_common)

set_target_properties(session_unlocker PROPERTIES
    OUTPUT_NAME "session_unlocker"
    PREFIX ""
)

if(WIN32 OR MINGW)
    target_compile_definitions(session_unlocker PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()
