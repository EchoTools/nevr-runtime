add_library(game_rules_override SHARED
    src/game_rules_override.cpp
    src/plugin_main.cpp
)

target_include_directories(game_rules_override PRIVATE src)
target_compile_definitions(game_rules_override PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(game_rules_override PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(game_rules_override PRIVATE minhook::minhook)

if(WIN32 OR MINGW)
    target_compile_definitions(game_rules_override PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(game_rules_override PROPERTIES
    OUTPUT_NAME "game_rules_override"
    PREFIX ""
)

if(BUILD_TESTING AND GTest_FOUND)
    add_executable(test_game_rules_override
        tests/test_config_parse.cpp
        src/game_rules_override.cpp
    )
    target_include_directories(test_game_rules_override PRIVATE src)
    target_link_libraries(test_game_rules_override PRIVATE nevr_plugin_common GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_game_rules_override)
endif()
