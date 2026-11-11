# gai_probe.exe: times the getaddrinfo call CBroadcaster::Initialize makes. tools/winvm/systest.py (scenario gai)
# copies the built file to the Windows VM and runs it there; it compiles nothing (ADR 0041). Written to
# <bin>/winvm/gai_probe.exe, the default of systest.py's --gai-probe.
add_executable(gai_probe gai_probe.c)
# The probe was an -O0 console program; the tree-wide -mwindows and -O3 come first on the command line, so these win.
target_compile_options(gai_probe PRIVATE -O0)
target_link_options(gai_probe PRIVATE -mconsole)
target_link_libraries(gai_probe PRIVATE ws2_32)
set_target_properties(gai_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/winvm")
