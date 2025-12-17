/**
 * @file telemetry_client.cpp
 * @brief Implementation of the telemetry HTTP client
 */

#include "telemetry_client.h"

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <sstream>

// Include generated protobuf headers
#include "rtapi/telemetry_v1.pb.h"

#pragma comment(lib, "winhttp.lib")

namespace TelemetryAgent {

TelemetryClient::TelemetryClient(const TelemetryClientConfig& config)
    : m_config(config),
      m_hSession(nullptr),
      m_hConnect(nullptr),
      m_running(false),
      m_port(80),
      m_secure(false),
      m_sentCount(0),
      m_failedCount(0) {}

TelemetryClient::~TelemetryClient() { Stop(); }

bool TelemetryClient::Start() {
  if (m_running) {
    return true;
  }

  // Parse the base URL
  if (!ParseUrl(m_config.baseUrl, m_host, m_port, m_path, m_secure)) {
    return false;
  }

  // Append the endpoint path
  if (m_path.empty() || m_path == "/") {
    m_path = "/lobby-session-events";
  } else {
    if (m_path.back() != '/') {
      m_path += '/';
    }
    m_path += "lobby-session-events";
  }

  // Create WinHTTP session
  m_hSession = WinHttpOpen(L"TelemetryAgent/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);

  if (!m_hSession) {
    return false;
  }

  // Set timeouts
  WinHttpSetTimeouts(m_hSession, m_config.timeoutMs, m_config.timeoutMs, m_config.timeoutMs, m_config.timeoutMs);

  // Convert host to wide string
  std::wstring wideHost(m_host.begin(), m_host.end());

  // Create connection handle
  m_hConnect = WinHttpConnect(m_hSession, wideHost.c_str(), m_port, 0);

  if (!m_hConnect) {
    WinHttpCloseHandle(m_hSession);
    m_hSession = nullptr;
    return false;
  }

  // Start sender thread
  m_running = true;
  m_senderThread = std::thread(&TelemetryClient::SenderThread, this);

  return true;
}

void TelemetryClient::Stop() {
  if (!m_running) {
    return;
  }

  m_running = false;
  m_queueCV.notify_all();

  if (m_senderThread.joinable()) {
    m_senderThread.join();
  }

  if (m_hConnect) {
    WinHttpCloseHandle(m_hConnect);
    m_hConnect = nullptr;
  }

  if (m_hSession) {
    WinHttpCloseHandle(m_hSession);
    m_hSession = nullptr;
  }

  // Clear the queue
  std::lock_guard<std::mutex> lock(m_queueMutex);
  while (!m_frameQueue.empty()) {
    m_frameQueue.pop();
  }
}

bool TelemetryClient::IsRunning() const { return m_running; }

bool TelemetryClient::SubmitFrame(const telemetry::LobbySessionStateFrame& frame) {
  if (!m_running) {
    return false;
  }

  // Skip frames with no events (to reduce network traffic)
  if (frame.events_size() == 0) {
    return true;
  }

  // Serialize to JSON
  std::string jsonData;
  google::protobuf::util::JsonPrintOptions jsonOptions;
  jsonOptions.add_whitespace = false;
  jsonOptions.always_print_fields_with_no_presence = false;
  jsonOptions.preserve_proto_field_names = true;

  auto status = google::protobuf::util::MessageToJsonString(frame, &jsonData, jsonOptions);
  if (!status.ok()) {
    ++m_failedCount;
    return false;
  }

  // Add to queue
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);

    if (m_frameQueue.size() >= m_config.maxQueueSize) {
      // Queue full, drop frame
      ++m_failedCount;
      return false;
    }

    m_frameQueue.push(std::move(jsonData));
  }

  m_queueCV.notify_one();
  return true;
}

size_t TelemetryClient::GetQueueDepth() const {
  std::lock_guard<std::mutex> lock(m_queueMutex);
  return m_frameQueue.size();
}

uint64_t TelemetryClient::GetSentCount() const { return m_sentCount; }

uint64_t TelemetryClient::GetFailedCount() const { return m_failedCount; }

void TelemetryClient::SenderThread() {
  while (m_running) {
    std::string jsonData;

    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_queueCV.wait_for(lock, std::chrono::milliseconds(100), [this] { return !m_frameQueue.empty() || !m_running; });

      if (!m_running && m_frameQueue.empty()) {
        break;
      }

      if (m_frameQueue.empty()) {
        continue;
      }

      jsonData = std::move(m_frameQueue.front());
      m_frameQueue.pop();
    }

    if (SendFrame(jsonData)) {
      ++m_sentCount;
    } else {
      ++m_failedCount;
    }
  }

  // Drain remaining frames
  while (true) {
    std::string jsonData;
    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      if (m_frameQueue.empty()) {
        break;
      }
      jsonData = std::move(m_frameQueue.front());
      m_frameQueue.pop();
    }

    if (SendFrame(jsonData)) {
      ++m_sentCount;
    } else {
      ++m_failedCount;
    }
  }
}

bool TelemetryClient::SendFrame(const std::string& jsonData) {
  if (!m_hConnect) {
    return false;
  }

  // Convert path to wide string
  std::wstring widePath(m_path.begin(), m_path.end());

  // Create request
  DWORD flags = m_secure ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET hRequest = WinHttpOpenRequest(m_hConnect, L"POST", widePath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

  if (!hRequest) {
    return false;
  }

  // Build headers
  std::wostringstream headers;
  headers << L"Content-Type: application/json\r\n";

  if (!m_config.userId.empty()) {
    std::wstring wideUserId(m_config.userId.begin(), m_config.userId.end());
    headers << L"X-User-ID: " << wideUserId << L"\r\n";
  }

  if (!m_config.nodeId.empty()) {
    std::wstring wideNodeId(m_config.nodeId.begin(), m_config.nodeId.end());
    headers << L"X-Node-ID: " << wideNodeId << L"\r\n";
  }

  std::wstring headerStr = headers.str();

  // Send request
  BOOL result =
      WinHttpSendRequest(hRequest, headerStr.c_str(), static_cast<DWORD>(headerStr.length()), (LPVOID)jsonData.c_str(),
                         static_cast<DWORD>(jsonData.size()), static_cast<DWORD>(jsonData.size()), 0);

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

  WinHttpCloseHandle(hRequest);

  // Accept 200-299 as success
  return statusCode >= 200 && statusCode < 300;
}

bool TelemetryClient::ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path,
                               bool& secure) {
  // Default values
  port = 80;
  secure = false;
  path = "/";

  std::string remaining = url;

  // Check for scheme
  if (remaining.compare(0, 8, "https://") == 0) {
    secure = true;
    port = 443;
    remaining = remaining.substr(8);
  } else if (remaining.compare(0, 7, "http://") == 0) {
    secure = false;
    port = 80;
    remaining = remaining.substr(7);
  }

  // Find path start
  size_t pathPos = remaining.find('/');
  std::string hostPort;

  if (pathPos != std::string::npos) {
    hostPort = remaining.substr(0, pathPos);
    path = remaining.substr(pathPos);
  } else {
    hostPort = remaining;
  }

  // Check for port
  size_t colonPos = hostPort.find(':');
  if (colonPos != std::string::npos) {
    host = hostPort.substr(0, colonPos);
    try {
      port = static_cast<uint16_t>(std::stoi(hostPort.substr(colonPos + 1)));
    } catch (...) {
      return false;
    }
  } else {
    host = hostPort;
  }

  return !host.empty();
}

}  // namespace TelemetryAgent
