# plugins/debug-lockout/CMakeLists.txt
# Debug instrumentation plugin — dumps early-quit lockout state.
# Add to plugins/CMakeLists.txt: add_subdirectory(debug-lockout)

add_library(nevr_debug_lockout SHARED
    src/plugin.cpp
)

target_compile_definitions(nevr_debug_lockout PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(nevr_debug_lockout PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(nevr_debug_lockout PRIVATE minhook::minhook)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(nevr_debug_lockout PRIVATE nlohmann_json::nlohmann_json)

if(WIN32 OR MINGW)
    target_compile_definitions(nevr_debug_lockout PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(nevr_debug_lockout PROPERTIES
    OUTPUT_NAME "nevr_debug_lockout"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
)
