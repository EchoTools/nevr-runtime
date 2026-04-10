# combat-mod plugin — combat mode for EchoVR arena levels

add_library(combat_mod SHARED
    src/plugin.cpp
    src/script_patch.cpp
    src/mode_patch.cpp
    src/combat_system.cpp
    src/start_visible.cpp
    src/level_detect.cpp
    src/resource_inject.cpp
)

target_compile_definitions(combat_mod PRIVATE NEVR_PLUGIN_EXPORTS)

target_include_directories(combat_mod PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(minhook CONFIG REQUIRED)

target_link_libraries(combat_mod PRIVATE
    nevr_plugin_common
    minhook::minhook
)

set_target_properties(combat_mod PROPERTIES
    OUTPUT_NAME "combat_mod"
    PREFIX ""
)

# Unit tests
if(BUILD_TESTING AND GTest_FOUND)
    add_executable(test_safe_memory
        tests/test_safe_memory.cpp
    )
    target_include_directories(test_safe_memory PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_safe_memory PRIVATE
        GTest::gtest GTest::gtest_main
    )
    gtest_discover_tests(test_safe_memory)
endif()
