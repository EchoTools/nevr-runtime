# echovr_server.exe — console CreateProcess wrapper for echovr.exe -server
# -headless -noconsole.
add_executable(echovr_server echovr_server_launcher.cpp)

# The launcher is intentionally a console application. The root sets the
# MSVC executable subsystem to WINDOWS, and its MinGW flags do not apply there.
if(MSVC)
    target_link_options(echovr_server PRIVATE /SUBSYSTEM:CONSOLE)
elseif(MINGW)
    target_link_options(echovr_server PRIVATE -mconsole -static -static-libgcc -static-libstdc++)
endif()
