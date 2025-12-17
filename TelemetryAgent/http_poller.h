#pragma once
/**
 * @file http_poller.h
 * @brief HTTP-based data source that polls /session and /player_bones endpoints
 *
 * This implementation polls the game server's built-in HTTP API endpoints
 * to retrieve session and player bones data for telemetry streaming.
 */

#ifndef TELEMETRY_HTTP_POLLER_H
#define TELEMETRY_HTTP_POLLER_H

#include <Windows.h>
#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <string>

#include "data_source.h"

namespace TelemetryAgent {

/**
 * @class HttpPoller
 * @brief Polls the game server's HTTP API for session and player bones data
 *
 * Uses WinHTTP for non-blocking HTTP requests to minimize impact on game performance.
 * The poller fetches from both /session and /player_bones endpoints concurrently.
 */
class HttpPoller : public IDataSource {
 public:
  /**
   * @brief Construct an HTTP poller for the given game server
   * @param host The hostname/IP of the game server (e.g., "127.0.0.1")
   * @param port The port number of the game server's HTTP API
   */
  HttpPoller(const std::string& host, uint16_t port);

  ~HttpPoller() override;

  // Disable copy
  HttpPoller(const HttpPoller&) = delete;
  HttpPoller& operator=(const HttpPoller&) = delete;

  // IDataSource interface
  bool Start() override;
  void Stop() override;
  bool IsActive() const override;
  bool GetFrameData(FrameData& data) override;
  std::string GetSessionUUID() const override;
  const char* GetName() const override { return "HTTP"; }

  /**
   * @brief Set the request timeout in milliseconds
   * @param timeoutMs Timeout value
   */
  void SetTimeout(uint32_t timeoutMs);

 private:
  /**
   * @brief Perform an HTTP GET request and return the response body
   * @param endpoint The endpoint path (e.g., "/session")
   * @param[out] response The response body
   * @return true if the request succeeded
   */
  bool HttpGet(const std::string& endpoint, std::string& response);

  /**
   * @brief Parse session metadata from JSON to extract session UUID
   * @param json The session JSON response
   * @return The session UUID, or empty string if not found
   */
  std::string ParseSessionUUID(const std::string& json);

 private:
  std::string m_host;
  uint16_t m_port;
  uint32_t m_timeoutMs;

  HINTERNET m_hSession;
  HINTERNET m_hConnect;

  std::atomic<bool> m_active;
  mutable std::mutex m_mutex;

  std::string m_cachedSessionUUID;
  uint64_t m_frameIndex;
};

}  // namespace TelemetryAgent

#endif  // TELEMETRY_HTTP_POLLER_H
