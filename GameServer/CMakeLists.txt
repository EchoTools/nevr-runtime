# GameServer CMakeLists.txt

# Game Server
set(SERVER_SOURCES "dllmain.cpp" "gameserver.cpp")
set(SERVER_HEADERS "gameserver.h")

add_library(GameServer SHARED ${SERVER_SOURCES} ${SERVER_HEADERS} "exports.def")

target_precompile_headers(GameServer PRIVATE "../include/pch.h")

target_include_directories(
  GameServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                     "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# Disable warning C4251: 'identifier': class 'type' needs to have dll-interface
# to be used by clients of class 'type2'

target_compile_options(GameServer PRIVATE /wd4251 /wd4267)
