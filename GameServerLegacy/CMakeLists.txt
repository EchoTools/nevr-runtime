# GameServerLegacy CMakeLists.txt
# Self-contained legacy implementation with local dependencies

set(SERVER_LEGACY_SOURCES
    "dllmain.cpp"
    "gameserver.cpp"
    "common/echovrInternal.cpp"
)

add_library(GameServerLegacy SHARED
    ${SERVER_LEGACY_SOURCES}
    "exports.def"
)

target_precompile_headers(GameServerLegacy PRIVATE "common/pch.h")

target_include_directories(
  GameServerLegacy
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
)

# No external dependencies - fully self-contained

# Disable specific warnings
target_compile_options(GameServerLegacy PRIVATE /wd4251 /wd4267)
