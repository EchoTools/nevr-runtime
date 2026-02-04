# GameServerLegacy CMakeLists.txt
# Self-contained legacy implementation with local dependencies

set(SERVER_LEGACY_SOURCES
    "dllmain.cpp"
    "gameserver.cpp"
    "common/echovr_internal.cpp"
)

add_library(gameserverlegacy SHARED
    ${SERVER_LEGACY_SOURCES}
    "exports.def"
)

target_precompile_headers(gameserverlegacy PRIVATE "common/pch.h")

target_include_directories(
  gameserverlegacy
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
)

# No external dependencies - fully self-contained

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
