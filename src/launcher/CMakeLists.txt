# echovr_server.exe — thin CreateProcess wrapper that spawns echovr.exe -server -noconsole.
# Overrides the global -mwindows link flag with -mconsole to produce a console application,
# so the user sees status messages before the game opens its own console window.
add_executable(echovr_server echovr_server_launcher.cpp)

# Override global -mwindows (set in root CMakeLists.txt for DLLs) with -mconsole.
# The launcher is intentionally a console application.
target_link_options(echovr_server PRIVATE -mconsole)

# Ensure static linking (inherited from global options but explicit for clarity).
target_link_options(echovr_server PRIVATE -static -static-libgcc -static-libstdc++)
