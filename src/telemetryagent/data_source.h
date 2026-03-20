#pragma once
/**
 * @file data_source.h
 * @brief Abstract interface for data sources (HTTP polling or direct memory access)
 *
 * This interface allows swapping between HTTP polling and direct memory reading
 * without changing the rest of the telemetry agent code.
 */

#ifndef TELEMETRY_DATA_SOURCE_H
#define TELEMETRY_DATA_SOURCE_H

#include <cstdint>
#include <memory>
#include <string>

namespace TelemetryAgent {

/**
 * @struct SessionData
 * @brief Raw session JSON data from the game server
 */
struct SessionData {
  std::string json;           // Raw JSON response from /session endpoint
  bool valid = false;         // Whether the data is valid/fresh
  uint64_t timestamp_ms = 0;  // Timestamp when data was captured
};

/**
 * @struct PlayerBonesData
 * @brief Raw player bones JSON data from the game server
 */
struct PlayerBonesData {
  std::string json;           // Raw JSON response from /player_bones endpoint
  bool valid = false;         // Whether the data is valid/fresh
  uint64_t timestamp_ms = 0;  // Timestamp when data was captured
};

/**
 * @struct FrameData
 * @brief Combined session and player bones data for a single frame
 */
struct FrameData {
  SessionData session;
  PlayerBonesData playerBones;
  uint64_t frameIndex = 0;
};

/**
 * @class IDataSource
 * @brief Abstract interface for retrieving game state data
 *
 * Implementations can either:
 * - Poll HTTP endpoints (/session, /player_bones)
 * - Read directly from game memory (future implementation)
 */
class IDataSource {
 public:
  virtual ~IDataSource() = default;

  /**
   * @brief Start the data source (connect, initialize, etc.)
   * @return true if successfully started
   */
  virtual bool Start() = 0;

  /**
   * @brief Stop the data source and release resources
   */
  virtual void Stop() = 0;

  /**
   * @brief Check if the data source is active and able to provide data
   * @return true if ready to provide data
   */
  virtual bool IsActive() const = 0;

  /**
   * @brief Get the current frame data
   * @param[out] data The frame data to populate
   * @return true if data was successfully retrieved
   */
  virtual bool GetFrameData(FrameData& data) = 0;

  /**
   * @brief Get the session UUID if available
   * @return Session UUID string, empty if not available
   */
  virtual std::string GetSessionUUID() const = 0;

  /**
   * @brief Get the name of this data source type
   * @return Name string (e.g., "HTTP", "Memory")
   */
  virtual const char* GetName() const = 0;
};

}  // namespace TelemetryAgent

#endif  // TELEMETRY_DATA_SOURCE_H
