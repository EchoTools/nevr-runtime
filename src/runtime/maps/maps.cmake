# The custom-map channel's runtime build (docs/adr/0047). Included from the end of src/runtime/CMakeLists.txt, after nevr_runtime
# and the test targets are defined. Each packet owns one fragment; a missing fragment adds nothing, so with none present the
# build is the build without the channel.
include(${CMAKE_CURRENT_LIST_DIR}/pc1.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc3.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc5.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc6.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc8.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc9.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/pc10.cmake OPTIONAL)
include(${CMAKE_CURRENT_LIST_DIR}/probes.cmake OPTIONAL)
