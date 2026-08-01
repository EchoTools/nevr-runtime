# plugins/example/CMakeLists.txt
# Minimal reference plugin — build target following the standard plugin conventions.
# Add to plugins/CMakeLists.txt: add_subdirectory(example)

add_library(nevr_example SHARED
    src/plugin.cpp
)

target_compile_definitions(nevr_example PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(nevr_example PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(nevr_example PRIVATE minhook::minhook)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(nevr_example PRIVATE nlohmann_json::nlohmann_json)

if(WIN32 OR MINGW)
    target_compile_definitions(nevr_example PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(nevr_example PROPERTIES
    OUTPUT_NAME "nevr_example"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)
