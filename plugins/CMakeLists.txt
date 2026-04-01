add_subdirectory(common)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        include(GoogleTest)
    endif()
endif()

# log-filter, server-timing are built into gamepatches.dll
# crash-handler functionality is in gamepatches core (crash_recovery.cpp)
# token-auth stays external: curl deps break dbgcore.dll loading under Wine
add_subdirectory(token-auth)
add_subdirectory(broadcaster-bridge)
add_subdirectory(audio-intercom)
add_subdirectory(game-rules-override)
add_subdirectory(session-unlocker)
add_subdirectory(skip-splash)
add_subdirectory(social-bridge)
add_subdirectory(filesystem-loader)
add_subdirectory(combat-patch)
add_subdirectory(pnsrad-enabler)
