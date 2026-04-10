# combat-mod plugin — combat mode for EchoVR arena levels
# Hooks game functions to enable combat chassis, weapons, body swap, respawn,
# and injects embedded resource data for the combat sublevel.

# Plugin sources will be added as modules are ported (Phase 4).
# For now, just the test targets.

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
