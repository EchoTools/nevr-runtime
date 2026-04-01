# Launcher — replaces echovr.exe
add_executable(echovr_launcher WIN32
    main.cpp
    pe_convert.cpp
)

target_compile_features(echovr_launcher PRIVATE cxx_std_17)
target_compile_definitions(echovr_launcher PRIVATE
    _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
)

# Use a different base address than the game (0x140000000) to avoid collision
if(MINGW)
    target_link_options(echovr_launcher PRIVATE -Wl,--image-base,0x10000000)
elseif(MSVC)
    target_link_options(echovr_launcher PRIVATE /BASE:0x10000000)
endif()

if(WIN32 OR MINGW)
    target_link_libraries(echovr_launcher PRIVATE psapi)
endif()

set_target_properties(echovr_launcher PROPERTIES
    OUTPUT_NAME "echovr_launcher"
    PREFIX ""
)

# Server wrapper
add_executable(echovr_server WIN32
    server_main.cpp
)

target_compile_features(echovr_server PRIVATE cxx_std_17)
target_compile_definitions(echovr_server PRIVATE
    _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
)

if(WIN32 OR MINGW)
    target_link_libraries(echovr_server PRIVATE shell32)
endif()

set_target_properties(echovr_server PROPERTIES
    OUTPUT_NAME "echovr_server"
    PREFIX ""
)
