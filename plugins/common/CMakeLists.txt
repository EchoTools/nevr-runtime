add_library(nevr_plugin_common INTERFACE)
target_include_directories(nevr_plugin_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/common
)
