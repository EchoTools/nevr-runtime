# The custom-map channel's core build (docs/adr/0047). Included from the end of src/core/CMakeLists.txt, after nevr_core is
# defined. Each packet owns one fragment; a missing fragment adds nothing, so with none present the build is the build without
# the channel. src/core/map_types.h is header-only and needs no line here.
include(${CMAKE_CURRENT_LIST_DIR}/pc4.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc5a.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc6core.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc8core.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc10core.cmake OPTIONAL)
