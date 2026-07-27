add_subdirectory(common)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        include(GoogleTest)
    endif()
endif()

# broadcaster-bridge MOVED to ~/src/nevr-runtime-plugins (2026-07-26): it is a
# broadcaster inject/mirror tool, and this repository is PUBLIC. Keeping the
# source here hands anyone a worked example of injecting into the broadcaster.
# anim-debugger MOVED to ~/src/nevr-runtime-plugins (2026-07-27): it is RE
# instrumentation that hooks three engine animation entry points and publishes a
# map of animation internals. It does nothing on a dedicated server, so this
# PUBLIC repo paid the disclosure cost for no runtime benefit.
# crash-handler functionality is in gamepatches core (crash_recovery.cpp)
# audio-intercom, combat-patch, filesystem-loader, force-push, game-rules-override,
# session-unlocker, skip-splash moved to nevr-runtime-plugins
add_subdirectory(log-filter)
add_subdirectory(example)
