add_library(combat_patch SHARED
    src/combat_patch.cpp
    src/plugin_main.cpp
)

target_include_directories(combat_patch PRIVATE src)
target_compile_definitions(combat_patch PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(combat_patch PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(combat_patch PRIVATE minhook::minhook)

if(WIN32 OR MINGW)
    target_compile_definitions(combat_patch PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
    # Disable stack protector — hook functions run inside the game's call
    # stack and the GS cookie check false-triggers on legitimate usage
    target_compile_options(combat_patch PRIVATE -fno-stack-protector)
endif()

set_target_properties(combat_patch PROPERTIES
    OUTPUT_NAME "combat_patch"
    PREFIX ""
)
