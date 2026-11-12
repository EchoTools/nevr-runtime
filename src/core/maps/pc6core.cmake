# CDN_PC PC6 (docs/adr/0047): the PE rules a map's script DLL must meet (core/script_image_rules.h), in nevr_core for
# the PC runtime and its test. Included through maps/maps.cmake from src/core/CMakeLists.txt, after nevr_core is
# defined.
# The ELF half for Quest is a later packet, so other platforms build nevr_core as before.
if(WIN32)
  target_sources(nevr_core PRIVATE "${CMAKE_SOURCE_DIR}/src/core/script_image_rules.cpp")
endif()
