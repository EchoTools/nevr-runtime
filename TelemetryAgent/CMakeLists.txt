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

add_library(TelemetryAgent SHARED
    ${TELEMETRY_SOURCES}
    ${TELEMETRY_HEADERS}
)

# Link against Windows HTTP services and threading
target_link_libraries(TelemetryAgent 
    PRIVATE 
    winhttp
    ws2_32
    proto_nevr
)

target_compile_definitions(TelemetryAgent 
    PRIVATE 
    TELEMETRY_AGENT_EXPORTS
    PROTOBUF_STATIC_LIB
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(TelemetryAgent
    PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
    "${CMAKE_SOURCE_DIR}/common"
    "${NEVR_COMMON_INCLUDE}"
)

# C++17 for std::optional, std::string_view, etc.
target_compile_features(TelemetryAgent PRIVATE cxx_std_17)

# Disable specific warnings
target_compile_options(TelemetryAgent PRIVATE /wd4251 /wd4267)
