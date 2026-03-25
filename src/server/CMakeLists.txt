# echovr_server — thin launcher that starts echovr.exe in dedicated server mode

add_executable(echovr_server "main.cpp")

set_target_properties(echovr_server PROPERTIES
  OUTPUT_NAME "echovr_server"
)

target_compile_features(echovr_server PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(echovr_server PRIVATE /MT)
  target_link_options(echovr_server PRIVATE /SUBSYSTEM:CONSOLE)
endif()

if(MINGW)
  target_compile_options(echovr_server PRIVATE -static-libgcc -static-libstdc++)
  # -Wl,--subsystem,console overrides the global -mwindows from the root CMakeLists
  target_link_options(echovr_server PRIVATE -static -static-libgcc -static-libstdc++ -mconsole -Wl,--subsystem,console)
endif()
