# GameServer CMakeLists.txt

# Game Server
set(SERVER_SOURCES "dllmain.cpp" "gameserver.cpp"
                   "${CMAKE_SOURCE_DIR}/common/echovrInternal.cpp")
set(SERVER_HEADERS "gameserver.h" "${CMAKE_SOURCE_DIR}/common/echovrInternal.h")

add_library(GameServer SHARED ${SERVER_SOURCES} ${SERVER_HEADERS} "exports.def")

target_precompile_headers(GameServer PRIVATE "${CMAKE_SOURCE_DIR}/common/pch.h")

target_include_directories(GameServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_directories(GameServer PRIVATE Common)
# Disable warning C4251: 'identifier': class 'type' needs to have dll-interface
# to be used by clients of class 'type2'

target_compile_options(GameServer PRIVATE /wd4251 /wd4267)
