add_library(nevr_crash_handler SHARED src/plugin.cpp)

target_include_directories(nevr_crash_handler PRIVATE src)
target_compile_definitions(nevr_crash_handler PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(nevr_crash_handler PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(nevr_crash_handler PRIVATE minhook::minhook)

set_target_properties(nevr_crash_handler PROPERTIES
    OUTPUT_NAME "crash_handler"
    PREFIX ""
)

if(WIN32 OR MINGW)
    target_compile_definitions(nevr_crash_handler PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()
