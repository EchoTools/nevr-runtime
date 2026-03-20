# pnsnevr - Nakama Social Platform Plugin for NEVR
# Prototype Implementation

# Don't create a new project since we're included as a subdirectory
# cmake_minimum_required(VERSION 3.16)
# project(pnsnevr VERSION 0.1.0 LANGUAGES CXX)

# set(CMAKE_CXX_STANDARD 17)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build as shared library (DLL on Windows)
add_library(pnsnevr SHARED
    src/dllmain.cpp
    src/nakama_client.cpp
    src/nakama_client.h
    src/game_bridge.cpp
    src/game_bridge.h
    src/config.cpp
    src/config.h
    src/message_handlers.cpp
    src/message_handlers.h
)

# Export symbols
target_compile_definitions(pnsnevr PRIVATE
    PNSNEVR_EXPORTS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

# Include nakama-cpp headers
# Note: nakama-cpp needs to be built separately first
# For now, we'll use a mock implementation for the prototype
target_include_directories(pnsnevr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/nakama-cpp/interface/include
)

# Platform libraries
if(WIN32)
    target_link_libraries(pnsnevr
        ws2_32
        winhttp
        crypt32
    )
endif()

# Install
install(TARGETS pnsnevr
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)

# For development: copy to game directory
# set(ECHOVR_GAME_DIR "C:/Program Files/Oculus/Software/ready-at-dawn-echo-arena")
# add_custom_command(TARGET pnsnevr POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:pnsnevr> "${ECHOVR_GAME_DIR}/pnsnevr.dll"
# )

message(STATUS "===========================================")
message(STATUS "  pnsnevr - Nakama Social Plugin")
message(STATUS "==========================================")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "===========================================")
