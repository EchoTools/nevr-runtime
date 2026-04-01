# TelemetryAgent CMakeLists.txt
#
# A DLL that polls the game server's HTTP API and streams telemetry to an external API server.
# This is designed to be loaded by pnsradgameserver.dll when a session starts.

set(TELEMETRY_SOURCES
    "telemetry_agent.cpp"
    "http_poller.cpp"
    "memory_poller.cpp"
    "telemetry_client.cpp"
    "frame_processor.cpp"
)

set(TELEMETRY_HEADERS
    "telemetry_agent.h"
    "http_poller.h"
    "memory_poller.h"
    "telemetry_client.h"
    "frame_processor.h"
    "data_source.h"
)

add_library(telemetryagent SHARED
    ${TELEMETRY_SOURCES}
    ${TELEMETRY_HEADERS}
)

# Link against Windows HTTP services and threading
target_link_libraries(telemetryagent 
    PRIVATE 
    winhttp
    ws2_32
    nevr_api
)

target_compile_definitions(telemetryagent 
    PRIVATE 
    TELEMETRY_AGENT_EXPORTS
    PROTOBUF_STATIC_LIB
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(telemetryagent
    PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
    "${CMAKE_SOURCE_DIR}/src/common"
    "${CMAKE_SOURCE_DIR}/gen/cpp"
)

# C++17 for std::optional, std::string_view, etc.
target_compile_features(telemetryagent PRIVATE cxx_std_17)

# Set output name to remove 'lib' prefix on MinGW
set_target_properties(telemetryagent PROPERTIES
  OUTPUT_NAME "telemetryagent"
  PREFIX ""
)

# Disable specific warnings (MSVC only)
if(MSVC)
  target_compile_options(telemetryagent PRIVATE /wd4251)
endif()
