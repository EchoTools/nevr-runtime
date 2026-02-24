# GameServer CMakeLists.txt

set(SERVER_SOURCES
    "dllmain.cpp"
    "messages.cpp"
    "gameserver.cpp"
    "servercontext.cpp"
    "websocket_client.cpp"
)

add_library(gameserver SHARED
    ${SERVER_SOURCES}
    "exports.def"
)

target_compile_definitions(gameserver PRIVATE PROTOBUF_STATIC_LIB)

target_precompile_headers(gameserver PRIVATE "${CMAKE_SOURCE_DIR}/src/common/pch.h")

target_include_directories(
  gameserver
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
          "${CMAKE_SOURCE_DIR}/src/common"
          "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(gameserver PRIVATE common proto_nevr telemetryagent ws2_32 ixwebsocket::ixwebsocket)

# Set output name to remove 'lib' prefix on MinGW
set_target_properties(gameserver PROPERTIES
  OUTPUT_NAME "gameserver"
  PREFIX ""
)

# Disable specific warnings
# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(gameserver PRIVATE /wd4251)
endif()

# Link ixwebsocket for WebSocket support
find_package(ixwebsocket CONFIG REQUIRED)

# Disable PCH for websocket_client.cpp to avoid Windows.h macro pollution
set_source_files_properties(websocket_client.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Fix missing mbedtls dependencies for MinGW
if(MINGW)
  target_link_options(gameserver PRIVATE
    -Wl,--allow-multiple-definition
  )
  # Add bcrypt explicitly - mbedcrypto needs BCryptGenRandom
  target_link_libraries(gameserver PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,/usr/x86_64-w64-mingw32/lib/libbcrypt.a>
  )
endif()

# Workaround: ixwebsocket package adds include/ixwebsocket to include path,
# but we need include/ since headers use #include <ixwebsocket/...>
get_target_property(_ixws_includes ixwebsocket::ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir ${_ixws_includes})
  get_filename_component(_parent_dir "${_dir}" DIRECTORY)
  target_include_directories(gameserver SYSTEM PRIVATE "${_parent_dir}")
endforeach()