/**
 * @file http_poller.cpp
 * @brief Implementation of HTTP-based data source
 */

#include "http_poller.h"

#include <chrono>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace TelemetryAgent {

HttpPoller::HttpPoller(const std::string& host, uint16_t port)
    : m_host(host),
      m_port(port),
      m_timeoutMs(3000),
      m_hSession(nullptr),
      m_hConnect(nullptr),
      m_active(false),
      m_frameIndex(0) {}

HttpPoller::~HttpPoller() { Stop(); }

bool HttpPoller::Start() {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_active) {
    return true;  // Already started
  }

  // Create WinHTTP session
  m_hSession = WinHttpOpen(L"TelemetryAgent/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);

  if (!m_hSession) {
    return false;
  }

  // Set timeouts
  WinHttpSetTimeouts(m_hSession, m_timeoutMs, m_timeoutMs, m_timeoutMs, m_timeoutMs);

  // Convert host to wide string
  std::wstring wideHost(m_host.begin(), m_host.end());

  // Create connection handle
  m_hConnect = WinHttpConnect(m_hSession, wideHost.c_str(), m_port, 0);

  if (!m_hConnect) {
    WinHttpCloseHandle(m_hSession);
    m_hSession = nullptr;
    return false;
  }

  m_active = true;
  m_frameIndex = 0;
  m_cachedSessionUUID.clear();

  return true;
}

void HttpPoller::Stop() {
  std::lock_guard<std::mutex> lock(m_mutex);

  m_active = false;

  if (m_hConnect) {
    WinHttpCloseHandle(m_hConnect);
    m_hConnect = nullptr;
  }

  if (m_hSession) {
    WinHttpCloseHandle(m_hSession);
    m_hSession = nullptr;
  }

  m_cachedSessionUUID.clear();
}

bool HttpPoller::IsActive() const { return m_active; }

bool HttpPoller::GetFrameData(FrameData& data) {
  if (!m_active) {
    return false;
  }

  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

  // Fetch session data
  std::string sessionJson;
  if (HttpGet("/session", sessionJson)) {
    data.session.json = std::move(sessionJson);
    data.session.valid = true;
    data.session.timestamp_ms = timestamp;

    // Update cached session UUID
    std::string uuid = ParseSessionUUID(data.session.json);
    if (!uuid.empty()) {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_cachedSessionUUID = uuid;
    }
  } else {
    data.session.valid = false;
    return false;
  }

  // Fetch player bones data
  std::string bonesJson;
  if (HttpGet("/player_bones", bonesJson)) {
    data.playerBones.json = std::move(bonesJson);
    data.playerBones.valid = true;
    data.playerBones.timestamp_ms = timestamp;
  } else {
    data.playerBones.valid = false;
    // Don't fail the whole frame just because bones failed
  }

  data.frameIndex = m_frameIndex++;
  return true;
}

std::string HttpPoller::GetSessionUUID() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_cachedSessionUUID;
}

void HttpPoller::SetTimeout(uint32_t timeoutMs) {
  m_timeoutMs = timeoutMs;
  if (m_hSession) {
    WinHttpSetTimeouts(m_hSession, m_timeoutMs, m_timeoutMs, m_timeoutMs, m_timeoutMs);
  }
}

bool HttpPoller::HttpGet(const std::string& endpoint, std::string& response) {
  if (!m_hConnect) {
    return false;
  }

  // Convert endpoint to wide string
  std::wstring wideEndpoint(endpoint.begin(), endpoint.end());

  // Create request
  HINTERNET hRequest = WinHttpOpenRequest(m_hConnect, L"GET", wideEndpoint.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, 0);

  if (!hRequest) {
    return false;
  }

  // Send request
  BOOL result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

  if (!result) {
    WinHttpCloseHandle(hRequest);
    return false;
  }

  // Wait for response
  result = WinHttpReceiveResponse(hRequest, nullptr);
  if (!result) {
    WinHttpCloseHandle(hRequest);
    return false;
  }

  // Check status code
  DWORD statusCode = 0;
  DWORD statusCodeSize = sizeof(statusCode);
  WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

  if (statusCode != 200) {
    WinHttpCloseHandle(hRequest);
    return false;
  }

  // Read response body
  response.clear();
  DWORD bytesAvailable = 0;

  do {
    bytesAvailable = 0;
    if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
      break;
    }

    if (bytesAvailable == 0) {
      break;
    }

    std::vector<char> buffer(bytesAvailable + 1);
    DWORD bytesRead = 0;

    if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
      buffer[bytesRead] = '\0';
      response.append(buffer.data(), bytesRead);
    }
  } while (bytesAvailable > 0);

  WinHttpCloseHandle(hRequest);
  return !response.empty();
}

std::string HttpPoller::ParseSessionUUID(const std::string& json) {
  // Simple JSON parsing for sessionid field
  // Format: "sessionid":"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
  const char* key = "\"sessionid\":\"";
  size_t pos = json.find(key);
  if (pos == std::string::npos) {
    return "";
  }

  pos += strlen(key);
  size_t endPos = json.find('"', pos);
  if (endPos == std::string::npos) {
    return "";
  }

  std::string uuid = json.substr(pos, endPos - pos);

  // Validate UUID format (basic check)
  if (uuid.length() == 36 && uuid[8] == '-' && uuid[13] == '-' && uuid[18] == '-' && uuid[23] == '-') {
    return uuid;
  }

  return "";
}

}  // namespace TelemetryAgent
