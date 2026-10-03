#pragma once

#include "core/logging.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace LogDiagnostics {

inline std::string FormatHttpResponseSummary(std::string_view prefix, long status, std::size_t responseBytes) {
  return std::string(prefix) + "http_status=" + std::to_string(status) +
         " response_bytes=" + std::to_string(responseBytes);
}

inline void LogHttpResponseSummary(EchoVR::LogLevel level, std::string_view prefix, long status,
                                   std::string_view response) {
  const std::string diagnostic = FormatHttpResponseSummary(prefix, status, response.size());
  Log(level, "%s", diagnostic.c_str());
}

inline std::string FormatCurlFailureDiagnostic(std::string_view prefix, int curlCode) {
  return std::string(prefix) + "curl_code=" + std::to_string(curlCode);
}

inline std::string FormatWebSocketCloseDiagnostic(std::string_view prefix, uint16_t closeCode,
                                                  uint32_t reconnectCount) {
  return std::string(prefix) + "code=" + std::to_string(closeCode) +
         " reconnect_count=" + std::to_string(reconnectCount);
}

inline std::string FormatWebSocketErrorDiagnostic(std::string_view prefix, int httpStatus, uint32_t retries,
                                                  uint32_t reconnectCount) {
  return std::string(prefix) + "http_status=" + std::to_string(httpStatus) +
         " retries=" + std::to_string(retries) + " reconnect_count=" + std::to_string(reconnectCount);
}

inline std::string FormatCallbackFailureDiagnostic(std::string_view callback) {
  return "[NEVR.WS] callback threw and was CONTAINED at=" + std::string(callback) +
         " failure=1 — server continues (an escape here reaches the game's unhandled-exception filter and kills it)";
}

inline std::string FormatLoginFailureDiagnostic(bool valid, uint64_t statusCode, size_t messageBytes,
                                                bool serverMode) {
  const std::string mode = serverMode
                               ? " (server mode: expected every boot — ServerDB never issues a real LoginSuccess; "
                                 "resolved with a synthesized LoginSuccess)"
                               : " (client mode: this is a real failure)";
  if (!valid) return "[NEVR.WS] login failed status=invalid message_bytes=0 diagnostic=invalid_frame" + mode;
  return "[NEVR.WS] login failed status=" + std::to_string(statusCode) +
         " message_bytes=" + std::to_string(messageBytes) + mode;
}

inline std::string FormatBindFailureDiagnostic(std::string_view listener, uint16_t port, int attempt,
                                               int maxAttempts) {
  return "[NEVR.WS] " + std::string(listener) + " port " + std::to_string(port) +
         " bind failed failure=1 — retrying (" +
         std::to_string(attempt) + "/" + std::to_string(maxAttempts) + ")";
}

}  // namespace LogDiagnostics
