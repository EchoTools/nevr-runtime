#pragma once
/**
 * @file frame_processor.h
 * @brief Processes raw frame data into telemetry protobuf messages
 *
 * Converts the raw JSON session and player bones data into structured
 * telemetry frames with event detection.
 */

#ifndef TELEMETRY_FRAME_PROCESSOR_H
#define TELEMETRY_FRAME_PROCESSOR_H

#include <memory>
#include <optional>
#include <string>

#include "data_source.h"

// Forward declarations for protobuf types
namespace telemetry {
namespace v1 {
class LobbySessionStateFrame;
}  // namespace v1
}  // namespace telemetry

namespace enginehttp {
class SessionResponse;
class PlayerBonesResponse;
}  // namespace enginehttp

namespace TelemetryAgent {

/**
 * @class FrameProcessor
 * @brief Converts raw frame data into telemetry protobuf messages
 *
 * This class handles:
 * - JSON parsing of session and player bones data
 * - Event detection by comparing consecutive frames
 * - Construction of telemetry protobuf messages
 */
class FrameProcessor {
 public:
  FrameProcessor();
  ~FrameProcessor();

  // Disable copy
  FrameProcessor(const FrameProcessor&) = delete;
  FrameProcessor& operator=(const FrameProcessor&) = delete;

  /**
   * @brief Process a frame of raw data into a telemetry frame
   * @param data Raw frame data from data source
   * @param[out] frame The resulting telemetry protobuf frame
   * @return true if processing succeeded
   */
  bool ProcessFrame(const FrameData& data, telemetry::v1::LobbySessionStateFrame& frame);

  /**
   * @brief Reset the processor state (call when session ends)
   */
  void Reset();

  /**
   * @brief Get the current session UUID
   * @return Session UUID or empty string
   */
  std::string GetSessionUUID() const;

 private:
  /**
   * @brief Parse session JSON into protobuf
   * @param json The session JSON string
   * @param[out] session The parsed session response
   * @return true if parsing succeeded
   */
  bool ParseSessionJson(const std::string& json, enginehttp::SessionResponse& session);

  /**
   * @brief Parse player bones JSON into protobuf
   * @param json The player bones JSON string
   * @param[out] bones The parsed player bones response
   * @return true if parsing succeeded
   */
  bool ParsePlayerBonesJson(const std::string& json, enginehttp::PlayerBonesResponse& bones);

  /**
   * @brief Detect events by comparing current and previous frames
   * @param current Current session state
   * @param frame The frame to add events to
   */
  void DetectEvents(const enginehttp::SessionResponse& current, telemetry::v1::LobbySessionStateFrame& frame);

 private:
  // Previous frame state for event detection
  std::unique_ptr<enginehttp::SessionResponse> m_previousSession;
  std::string m_sessionUUID;
  uint32_t m_frameIndex;
};

}  // namespace TelemetryAgent

#endif  // TELEMETRY_FRAME_PROCESSOR_H
