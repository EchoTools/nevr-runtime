add_library(social_bridge SHARED
    src/plugin.cpp
    src/social_bridge.cpp
    src/nevr_client.cpp
)

target_include_directories(social_bridge PRIVATE src)
target_compile_definitions(social_bridge PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(social_bridge PRIVATE nevr_plugin_common)

find_package(CURL REQUIRED)
target_link_libraries(social_bridge PRIVATE CURL::libcurl)

if(WIN32 OR MINGW)
    target_compile_definitions(social_bridge PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(social_bridge PROPERTIES
    OUTPUT_NAME "social_bridge"
    PREFIX ""
)
