# ws-bridge module — In-process WebSocket TLS proxy.
# Bridges ws:// (game) to wss:// (server) using ixwebsocket (mbedTLS).

find_package(ixwebsocket CONFIG REQUIRED)

add_library(ws_bridge SHARED
    src/ws_bridge.cpp
)

target_compile_definitions(ws_bridge PRIVATE NEVR_MODULE_EXPORTS)

target_include_directories(ws_bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/gamepatches
)

# Disable PCH — ixwebsocket headers conflict with Windows.h macros
# target_precompile_headers(ws_bridge PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")

target_link_libraries(ws_bridge PRIVATE common ixwebsocket::ixwebsocket)
target_link_directories(ws_bridge PRIVATE common)

# ixwebsocket include path workaround
get_target_property(_ixws_includes ixwebsocket::ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir ${_ixws_includes})
  get_filename_component(_parent_dir "${_dir}" DIRECTORY)
  target_include_directories(ws_bridge SYSTEM PRIVATE "${_parent_dir}")
endforeach()

# MinGW: mbedTLS needs BCryptGenRandom
if(MINGW)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/stubs"
    COMMAND x86_64-w64-mingw32-dlltool -d "${CMAKE_SOURCE_DIR}/src/gamepatches/bcrypt_minimal.def"
            -l "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a"
    DEPENDS "${CMAKE_SOURCE_DIR}/src/gamepatches/bcrypt_minimal.def"
    COMMENT "Generating minimal bcrypt import library for ws_bridge"
  )
  add_custom_target(ws_bridge_bcrypt_minimal DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a")
  add_dependencies(ws_bridge ws_bridge_bcrypt_minimal)
  target_link_libraries(ws_bridge PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a>
  )
endif()

set_target_properties(ws_bridge PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules"
)
