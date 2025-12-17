#pragma once
/**
 * @file telemetry_agent.h
 * @brief Main telemetry agent DLL interface
 *
 * This DLL provides telemetry streaming capabilities for the game server.
 * It polls the game's HTTP API (or reads directly from memory) and streams
 * session state and player bones data to an external telemetry API server.
 *
 * The agent runs in its own thread to avoid blocking the game and is designed
 * to only run during active game sessions.
 *
 * Usage:
 *   1. Call TelemetryAgent_Initialize() when the game server loads
 *   2. Call TelemetryAgent_StartSession() when a game session begins
 *   3. Call TelemetryAgent_StopSession() when the session ends
 *   4. Call TelemetryAgent_Shutdown() when the game server unloads
 */

#ifndef TELEMETRY_AGENT_H
#define TELEMETRY_AGENT_H

#include <Windows.h>

#include <cstdint>

#ifdef TELEMETRY_AGENT_EXPORTS
#define TELEMETRY_API __declspec(dllexport)
#else
#define TELEMETRY_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum TelemetryDataSourceType
 * @brief Type of data source to use for retrieving game state
 */
typedef enum {
  TELEMETRY_SOURCE_HTTP = 0,    ///< Poll HTTP API endpoints (default)
  TELEMETRY_SOURCE_MEMORY = 1,  ///< Read directly from game memory (placeholder)
} TelemetryDataSourceType;

/**
 * @struct TelemetryAgentConfig
 * @brief Configuration for the telemetry agent
 */
typedef struct {
  /// Polling frequency in Hz (default: 10)
  uint32_t pollingFrequencyHz;

  /// Data source type (HTTP or Memory)
  TelemetryDataSourceType dataSource;

  /// Game server HTTP API host (default: "127.0.0.1")
  const char* gameServerHost;

  /// Game server HTTP API port (default: 6721)
  uint16_t gameServerPort;

  /// Telemetry API server URL (default: "http://localhost:8081")
  const char* telemetryApiUrl;

  /// Optional user ID for telemetry API
  const char* userId;

  /// Node ID for telemetry API (default: "game-server")
  const char* nodeId;

  /// Game base address for memory-based polling (0 for HTTP)
  uintptr_t gameBaseAddress;

} TelemetryAgentConfig;

/**
 * @brief Get default configuration values
 * @param[out] config Configuration structure to fill with defaults
 */
TELEMETRY_API void TelemetryAgent_GetDefaultConfig(TelemetryAgentConfig* config);

/**
 * @brief Initialize the telemetry agent
 * @param config Configuration for the agent (NULL for defaults)
 * @return 0 on success, non-zero error code on failure
 *
 * This should be called once when the game server DLL is loaded.
 * It does NOT start the polling/streaming - call TelemetryAgent_StartSession() for that.
 */
TELEMETRY_API int TelemetryAgent_Initialize(const TelemetryAgentConfig* config);

/**
 * @brief Check if the agent is initialized
 * @return Non-zero if initialized, 0 if not
 */
TELEMETRY_API int TelemetryAgent_IsInitialized(void);

/**
 * @brief Start a telemetry session
 * @return 0 on success, non-zero error code on failure
 *
 * Starts the background polling thread and telemetry streaming.
 * Call this when a game session begins.
 */
TELEMETRY_API int TelemetryAgent_StartSession(void);

/**
 * @brief Stop the current telemetry session
 * @return 0 on success, non-zero error code on failure
 *
 * Stops the background polling thread and flushes any pending telemetry.
 * Call this when a game session ends.
 */
TELEMETRY_API int TelemetryAgent_StopSession(void);

/**
 * @brief Check if a telemetry session is active
 * @return Non-zero if session is active, 0 if not
 */
TELEMETRY_API int TelemetryAgent_IsSessionActive(void);

/**
 * @brief Shutdown the telemetry agent
 * @return 0 on success, non-zero error code on failure
 *
 * Stops any active session and releases all resources.
 * Call this when the game server DLL is unloaded.
 */
TELEMETRY_API int TelemetryAgent_Shutdown(void);

/**
 * @brief Get the current session UUID
 * @param[out] buffer Buffer to write the session UUID to
 * @param bufferSize Size of the buffer
 * @return Length of the UUID string, or 0 if no session
 */
TELEMETRY_API int TelemetryAgent_GetSessionUUID(char* buffer, uint32_t bufferSize);

/**
 * @brief Get telemetry statistics
 * @param[out] framesPolled Number of frames polled
 * @param[out] framesSent Number of frames sent to API
 * @param[out] framesFailed Number of failed frame submissions
 * @param[out] queueDepth Current queue depth
 */
TELEMETRY_API void TelemetryAgent_GetStats(uint64_t* framesPolled, uint64_t* framesSent, uint64_t* framesFailed,
                                           uint32_t* queueDepth);

/**
 * @brief Set the telemetry API URL at runtime
 * @param url New API URL
 * @return 0 on success, non-zero if session is active (can't change during session)
 */
TELEMETRY_API int TelemetryAgent_SetApiUrl(const char* url);

/**
 * @brief Set the polling frequency at runtime
 * @param frequencyHz New frequency in Hz
 * @return 0 on success
 */
TELEMETRY_API int TelemetryAgent_SetPollingFrequency(uint32_t frequencyHz);

#ifdef __cplusplus
}
#endif

#endif  // TELEMETRY_AGENT_H
