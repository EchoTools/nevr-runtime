# GameServer CMakeLists.txt

set(SERVER_SOURCES
    "dllmain.cpp"
    "messages.cpp"
    "gameserver.cpp"
)
# Header files do not need to be listed in a variable for compilation.
# set(SERVER_HEADERS ...) can be removed or used just for IDEs, but not in add_library.

# CORRECTED: The ${SERVER_HEADERS} variable has been removed from this command.
add_library(GameServer SHARED
    ${SERVER_SOURCES}
    "exports.def"
)

target_compile_definitions(GameServer PRIVATE PROTOBUF_STATIC_LIB)

target_precompile_headers(GameServer PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")

# These commands correctly tell the compiler where to find headers.
target_include_directories(
  GameServer
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
          "${CMAKE_SOURCE_DIR}/common"
          "${NEVR_COMMON_INCLUDE}"
)

# This command correctly tells the linker what to link against.
target_link_libraries(GameServer PRIVATE Common proto_nevr)

# Disable specific warnings
target_compile_options(GameServer PRIVATE /wd4251 /wd4267)