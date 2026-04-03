add_subdirectory(common)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        include(GoogleTest)
    endif()
endif()

# log-filter, server-timing, token-auth are built into gamepatches.dll
# crash-handler functionality is in gamepatches core (crash_recovery.cpp)
# audio-intercom, combat-patch, filesystem-loader, force-push, game-rules-override,
# session-unlocker, skip-splash moved to nevr-runtime-plugins
add_subdirectory(broadcaster-bridge)
add_subdirectory(social-bridge)
add_subdirectory(pnsrad-enabler)
