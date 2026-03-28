add_library(broadcaster_bridge SHARED
    src/broadcaster_bridge.cpp
    src/plugin_main.cpp
)

target_include_directories(broadcaster_bridge PRIVATE src)
target_compile_definitions(broadcaster_bridge PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(broadcaster_bridge PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(broadcaster_bridge PRIVATE minhook::minhook)

if(WIN32 OR MINGW)
    target_link_libraries(broadcaster_bridge PRIVATE ws2_32)
    target_compile_definitions(broadcaster_bridge PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
    )
endif()

set_target_properties(broadcaster_bridge PROPERTIES
    OUTPUT_NAME "broadcaster_bridge"
    PREFIX ""
)

if(BUILD_TESTING AND GTest_FOUND)
    add_executable(test_broadcaster_bridge
        tests/test_wire_format.cpp
        src/broadcaster_bridge.cpp
    )
    target_include_directories(test_broadcaster_bridge PRIVATE src)
    target_link_libraries(test_broadcaster_bridge PRIVATE nevr_plugin_common GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_broadcaster_bridge)
endif()
