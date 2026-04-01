add_library(pnsrad_enabler SHARED src/plugin.cpp)

target_include_directories(pnsrad_enabler PRIVATE src)
target_compile_definitions(pnsrad_enabler PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(pnsrad_enabler PRIVATE nevr_plugin_common)

set_target_properties(pnsrad_enabler PROPERTIES
    OUTPUT_NAME "pnsrad_enabler"
    PREFIX ""
)

if(WIN32 OR MINGW)
    target_compile_definitions(pnsrad_enabler PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()
