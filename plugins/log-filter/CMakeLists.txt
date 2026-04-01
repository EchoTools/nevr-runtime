add_library(log_filter SHARED
    src/plugin.cpp
    src/log_filter.cpp
)

target_include_directories(log_filter PRIVATE src)
target_compile_definitions(log_filter PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(log_filter PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(log_filter PRIVATE minhook::minhook nlohmann_json::nlohmann_json)

if(WIN32 OR MINGW)
    target_compile_definitions(log_filter PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(log_filter PROPERTIES
    OUTPUT_NAME "log_filter"
    PREFIX ""
)
