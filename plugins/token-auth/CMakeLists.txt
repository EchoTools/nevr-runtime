add_library(token_auth SHARED
    src/plugin.cpp
    src/device_auth.cpp
)

target_include_directories(token_auth PRIVATE src)
target_compile_definitions(token_auth PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(token_auth PRIVATE nevr_plugin_common)

find_package(CURL REQUIRED)
target_link_libraries(token_auth PRIVATE CURL::libcurl)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(token_auth PRIVATE nlohmann_json::nlohmann_json)

if(WIN32 OR MINGW)
    target_compile_definitions(token_auth PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(token_auth PROPERTIES
    OUTPUT_NAME "token_auth"
    PREFIX ""
)
