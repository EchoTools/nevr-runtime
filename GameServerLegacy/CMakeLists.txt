# GameServerLegacy CMakeLists.txt

set(SERVER_LEGACY_SOURCES
    "dllmain.cpp"
    "gameserver.cpp"
)

add_library(GameServerLegacy SHARED
    ${SERVER_LEGACY_SOURCES}
    "exports.def"
)

target_precompile_headers(GameServerLegacy PRIVATE "${NEVR_COMMON_INCLUDE}/pch.h")

target_include_directories(
  GameServerLegacy
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
          "${CMAKE_SOURCE_DIR}/common"
          "${CMAKE_SOURCE_DIR}"
          "${NEVR_COMMON_INCLUDE}"
)

target_link_libraries(GameServerLegacy PRIVATE Common)

# Disable specific warnings
target_compile_options(GameServerLegacy PRIVATE /wd4251 /wd4267)
