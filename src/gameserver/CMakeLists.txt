# GameServer CMakeLists.txt

set(SERVER_SOURCES
    "dllmain.cpp"
    "messages.cpp"
    "gameserver.cpp"
    "server_context.cpp"
    "websocket_client.cpp"
    "telemetry_streamer.cpp"
    "upnp.cpp"
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

target_link_libraries(gameserver PRIVATE common proto_nevr ws2_32 ixwebsocket::ixwebsocket miniupnpc::miniupnpc)

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

# Link miniupnpc for UPnP port forwarding
find_package(miniupnpc CONFIG REQUIRED)

# Disable PCH for websocket_client.cpp, telemetry_streamer.cpp, and upnp.cpp to avoid Windows.h macro pollution
set_source_files_properties(websocket_client.cpp telemetry_streamer.cpp upnp.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Fix missing mbedtls dependencies for MinGW
if(MINGW)
  target_link_options(gameserver PRIVATE
    -Wl,--allow-multiple-definition
  )

  # Abseil's symbolize library pulls in -ldbghelp, but the real dbghelp.dll
  # depends on dbgcore.dll which conflicts with our gamepatches DLL hijack.
  # Build a stub libdbghelp.a that the linker finds before the system one,
  # providing no-op implementations so dbghelp.dll is never imported.
  add_library(dbghelp_stubs STATIC dbghelp_stubs.cpp)
  target_compile_definitions(dbghelp_stubs PRIVATE PROTOBUF_STATIC_LIB)
  set_target_properties(dbghelp_stubs PROPERTIES
    OUTPUT_NAME "dbghelp"
    PREFIX "lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stubs"
  )
  # Ensure our stub directory is searched before the system MinGW libs
  target_link_directories(gameserver BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/stubs")
  add_dependencies(gameserver dbghelp_stubs)

  # mbedcrypto needs BCryptGenRandom from bcrypt.dll. The system import lib
  # with WHOLE_ARCHIVE imports ~50 functions (some missing on older Windows).
  # Build a minimal import lib from a .def with only BCryptGenRandom, then
  # WHOLE_ARCHIVE it so the symbol resolves regardless of link order.
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/stubs"
    COMMAND x86_64-w64-mingw32-dlltool -d "${CMAKE_CURRENT_SOURCE_DIR}/bcrypt_minimal.def"
            -l "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bcrypt_minimal.def"
    COMMENT "Generating minimal bcrypt import library"
  )
  add_custom_target(bcrypt_minimal DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a")
  add_dependencies(gameserver bcrypt_minimal)
  target_link_libraries(gameserver PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_CURRENT_BINARY_DIR}/stubs/libbcrypt.a>
  )
endif()

# Workaround: ixwebsocket package adds include/ixwebsocket to include path,
# but we need include/ since headers use #include <ixwebsocket/...>
get_target_property(_ixws_includes ixwebsocket::ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir ${_ixws_includes})
  get_filename_component(_parent_dir "${_dir}" DIRECTORY)
  target_include_directories(gameserver SYSTEM PRIVATE "${_parent_dir}")
endforeach()
