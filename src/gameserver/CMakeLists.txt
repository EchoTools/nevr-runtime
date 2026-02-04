# GameServer CMakeLists.txt

set(SERVER_SOURCES
    "dllmain.cpp"
    "messages.cpp"
    "gameserver.cpp"
    "servercontext.cpp"
)

add_library(gameserver SHARED
    ${SERVER_SOURCES}
    "exports.def"
)

target_compile_definitions(gameserver PRIVATE PROTOBUF_STATIC_LIB)

target_precompile_headers(gameserver PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")

target_include_directories(
  gameserver
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
          "${CMAKE_SOURCE_DIR}/src/common"
          "${CMAKE_SOURCE_DIR}"
          "${NEVR_COMMON_INCLUDE}"
)

target_link_libraries(gameserver PRIVATE common proto_nevr telemetryagent ws2_32)

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