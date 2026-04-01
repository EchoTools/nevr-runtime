# pnsrad.dll — RAD social platform plugin reconstruction
# Logically identical to the original pnsrad.dll from EchoVR
#
# Status: INCOMPLETE RECONSTRUCTION
# ~300 internal symbols remain unresolved (error handling, string helpers,
# allocator, config system, etc.). These are functions within pnsrad.dll
# itself that haven't been reconstructed yet — not host imports.
#
# MinGW's ld ignores --unresolved-symbols=ignore-all for PE/COFF targets,
# so we build as OBJECT to verify compilation without linking.
# Switch back to SHARED once stubs or implementations exist for all symbols.

file(GLOB_RECURSE PNSRAD_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(pnsrad OBJECT ${PNSRAD_SOURCES})

target_include_directories(pnsrad PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(pnsrad PRIVATE PNSRAD_EXPORTS)

# Dependencies (headers only — no linking for OBJECT libraries)
find_package(Opus REQUIRED)
target_link_libraries(pnsrad PRIVATE Opus::opus)

if(WIN32 OR MINGW)
  target_link_libraries(pnsrad PRIVATE ws2_32)
endif()
