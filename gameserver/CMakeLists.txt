# GameServer CMakeLists.txt

set(SERVER_SOURCES
    "dllmain.cpp"
    "messages.cpp"
    "gameserver.cpp"
    "servercontext.cpp"
)

add_library(GameServer SHARED
    ${SERVER_SOURCES}
    "exports.def"
)

target_compile_definitions(GameServer PRIVATE PROTOBUF_STATIC_LIB)

target_precompile_headers(GameServer PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")

target_include_directories(
  GameServer
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
          "${CMAKE_SOURCE_DIR}/common"
          "${CMAKE_SOURCE_DIR}"
          "${NEVR_COMMON_INCLUDE}"
)

target_link_libraries(GameServer PRIVATE Common proto_nevr TelemetryAgent)

# Disable specific warnings
target_compile_options(GameServer PRIVATE /wd4251 /wd4267)