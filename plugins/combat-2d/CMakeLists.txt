# combat-2d plugin — PC/2D mode fixes (hand position, VR flag, AFK)

add_library(combat_2d SHARED
    src/plugin.cpp
)

target_compile_definitions(combat_2d PRIVATE NEVR_PLUGIN_EXPORTS)

target_link_libraries(combat_2d PRIVATE
    nevr_plugin_common
)

set_target_properties(combat_2d PROPERTIES
    OUTPUT_NAME "combat_2d"
    PREFIX ""
)
