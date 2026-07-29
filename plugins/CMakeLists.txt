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
# log-filter is NOT built (N95). plugin_loader.cpp refuses log_filter.dll by
# name — loading it installs a second MinHook on CLog::PrintfImpl from a
# different static MinHook copy, which took the built-in filter's game-line
# capture to zero (N89). Producing a DLL our own loader is guaranteed to reject
# is the defect. The SOURCE stays: its history carries fixes not yet audited
# against src/runtime/log/builtin_filter.cpp, and deleting first is what
# stranded a fix twice here (N93, N94).
add_subdirectory(example)
