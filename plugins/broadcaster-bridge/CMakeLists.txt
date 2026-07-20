add_library(broadcaster_bridge SHARED
    src/broadcaster_bridge.cpp
    src/plugin_main.cpp
)

target_include_directories(broadcaster_bridge PRIVATE src)
target_compile_definitions(broadcaster_bridge PRIVATE NEVR_PLUGIN_EXPORTS)
target_link_libraries(broadcaster_bridge PRIVATE nevr_plugin_common)

find_package(minhook CONFIG REQUIRED)
target_link_libraries(broadcaster_bridge PRIVATE minhook::minhook)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(broadcaster_bridge PRIVATE nlohmann_json::nlohmann_json)

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
    # broadcaster_bridge.cpp pulls in winsock (htons/socket/WSAStartup/...) and MinHook
    # (MH_*); the test must link the same libraries the shared target does (minhook + ws2_32
    # on Windows/MinGW), or it fails at link with undefined __imp_* / MH_* references.
    target_link_libraries(test_broadcaster_bridge PRIVATE
        nevr_plugin_common minhook::minhook GTest::gtest GTest::gtest_main)
    if(WIN32 OR MINGW)
        target_link_libraries(test_broadcaster_bridge PRIVATE ws2_32)
        target_compile_definitions(test_broadcaster_bridge PRIVATE
            _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
        )
    endif()
    # PRE_TEST: defer discovery to ctest time so `cmake --build` does not launch the
    # cross-compiled .exe under Wine with a 5s timeout (Wine cold-start exceeds it).
    gtest_discover_tests(test_broadcaster_bridge DISCOVERY_MODE PRE_TEST)
endif()
