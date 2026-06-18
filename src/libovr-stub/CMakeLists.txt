# LibOVRPlatform64_1.dll — stub for pnsovr.dll to load without real Oculus SDK
# Cross-compilable with MinGW (x86_64-w64-mingw32-g++)

add_library(LibOVRPlatform64_1 SHARED
    stub.cpp
)

# Output name: LibOVRPlatform64_1.dll (no "lib" prefix on MinGW)
set_target_properties(LibOVRPlatform64_1 PROPERTIES
    OUTPUT_NAME "LibOVRPlatform64_1"
    PREFIX ""                     # MinGW defaults to "lib" prefix — suppress it
    SUFFIX ".dll"
)

# Use the .def file for controlled exports
if(MINGW)
    target_link_options(LibOVRPlatform64_1 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/exports.def"
    )
elseif(MSVC)
    target_sources(LibOVRPlatform64_1 PRIVATE exports.def)
endif()

target_compile_definitions(LibOVRPlatform64_1 PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
