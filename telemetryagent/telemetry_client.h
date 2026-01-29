#pragma once
/**
 * @file telemetry_client.h
 * @brief HTTP client for sending telemetry frames to the API server
 *
 * Implements non-blocking telemetry submission with a background queue
 * to avoid blocking the game thread.
 */

#ifndef TELEMETRY_CLIENT_H
#define TELEMETRY_CLIENT_H

#include <Windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// Forward declaration
namespace telemetry {
namespace v1 {
class LobbySessionStateFrame;
}  // namespace v1
}  // namespace telemetry

namespace TelemetryAgent {

/**
 * @struct TelemetryClientConfig
 * @brief Configuration for the telemetry client
 */
struct TelemetryClientConfig {
  std::string baseUrl = "http://localhost:8081";  // Base URL of the events API
  std::string userId;                             // Optional user ID header
  std::string nodeId = "default-node";            // Node ID header
  uint32_t timeoutMs = 5000;                      // HTTP request timeout
  size_t maxQueueSize = 1000;                     // Maximum frames to queue before dropping
};

/**
 * @class TelemetryClient
 * @brief Sends telemetry frames to the API server
 *
 * Features:
 * - Non-blocking frame submission via background thread
 * - Automatic JSON serialization of protobuf frames
 * - Configurable timeouts and queue sizes
 * - Graceful shutdown with queue draining
 */
class TelemetryClient {
 public:
  /**
   * @brief Construct a telemetry client with the given configuration
   * @param config Client configuration
   */
  explicit TelemetryClient(const TelemetryClientConfig& config);

  ~TelemetryClient();

  // Disable copy
  TelemetryClient(const TelemetryClient&) = delete;
  TelemetryClient& operator=(const TelemetryClient&) = delete;

  /**
   * @brief Start the client and background sender thread
   * @return true if started successfully
   */
  bool Start();

  /**
   * @brief Stop the client and drain the queue
   */
  void Stop();

  /**
   * @brief Check if the client is running
   * @return true if running
   */
  bool IsRunning() const;

  /**
   * @brief Submit a frame for sending (non-blocking)
   * @param frame The telemetry frame to send
   * @return true if queued successfully, false if queue is full
   */
  bool SubmitFrame(const telemetry::v1::LobbySessionStateFrame& frame);

  /**
   * @brief Get the current queue depth
   * @return Number of frames waiting to be sent
   */
  size_t GetQueueDepth() const;

  /**
   * @brief Get count of successfully sent frames
   * @return Number of frames sent
   */
  uint64_t GetSentCount() const;

  /**
   * @brief Get count of failed frame submissions
   * @return Number of failures
   */
  uint64_t GetFailedCount() const;

 private:
  /**
   * @brief Background sender thread function
   */
  void SenderThread();

  /**
   * @brief Send a single frame to the API server
   * @param jsonData JSON-serialized frame data
   * @return true if sent successfully
   */
  bool SendFrame(const std::string& jsonData);

  /**
   * @brief Parse URL into components
   * @param url The URL to parse
   * @param[out] host Host component
   * @param[out] port Port number
   * @param[out] path Path component
   * @param[out] secure Whether HTTPS
   * @return true if parsed successfully
   */
  bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path, bool& secure);

 private:
  TelemetryClientConfig m_config;

  HINTERNET m_hSession;
  HINTERNET m_hConnect;

  std::atomic<bool> m_running;
  std::thread m_senderThread;

  mutable std::mutex m_queueMutex;
  std::condition_variable m_queueCV;
  std::queue<std::string> m_frameQueue;  // JSON-serialized frames

  std::atomic<uint64_t> m_sentCount;
  std::atomic<uint64_t> m_failedCount;

  // Parsed URL components
  std::string m_host;
  uint16_t m_port;
  std::string m_path;
  bool m_secure;
};

}  // namespace TelemetryAgent

#endif  // TELEMETRY_CLIENT_H
