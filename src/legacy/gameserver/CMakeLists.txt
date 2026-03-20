# GameServerLegacy CMakeLists.txt
# Self-contained legacy implementation with local dependencies

set(SERVER_LEGACY_SOURCES
    "dllmain.cpp"
    "gameserver.cpp"
    "websocket_client.cpp"
    "common/echovr_internal.cpp"
)

add_library(gameserverlegacy SHARED
    ${SERVER_LEGACY_SOURCES}
    "exports.def"
)

target_precompile_headers(gameserverlegacy PRIVATE "common/pch.h")

# Disable PCH for websocket_client.cpp to avoid Windows.h macro pollution
set_source_files_properties(websocket_client.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_include_directories(
  gameserverlegacy
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
)

# Link ixwebsocket for WebSocket support
find_package(ixwebsocket CONFIG REQUIRED)
target_link_libraries(gameserverlegacy PRIVATE ixwebsocket::ixwebsocket)

# Fix missing mbedtls dependencies for MinGW
if(MINGW)
  target_link_options(gameserverlegacy PRIVATE 
    -Wl,--allow-multiple-definition
  )
  # Add bcrypt explicitly - mbedcrypto needs BCryptGenRandom
  # Must come AFTER mbedcrypto in link order, so add to end
  target_link_libraries(gameserverlegacy PRIVATE 
    $<LINK_LIBRARY:WHOLE_ARCHIVE,/usr/x86_64-w64-mingw32/lib/libbcrypt.a>
  )
endif()

# Workaround: ixwebsocket package adds include/ixwebsocket to include path,
# but we need include/ since headers use #include <ixwebsocket/...>
get_target_property(_ixws_includes ixwebsocket::ixwebsocket INTERFACE_INCLUDE_DIRECTORIES)
foreach(_dir ${_ixws_includes})
  get_filename_component(_parent_dir "${_dir}" DIRECTORY)
  target_include_directories(gameserverlegacy SYSTEM PRIVATE "${_parent_dir}")
endforeach()

# Set output name to remove 'lib' prefix on MinGW
set_target_properties(gameserverlegacy PROPERTIES
  OUTPUT_NAME "gameserverlegacy"
  PREFIX ""
)

# Disable specific warnings
# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(gameserverlegacy PRIVATE /wd4251 /wd4267)
endif()
