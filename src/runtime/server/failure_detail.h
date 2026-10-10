#pragma once

// Operator-facing cause text for the two ServerFatal sites in gameserver_callbacks.cpp and gameserver_serverdb.cpp, which must say more
// than "failed" (#35): the registration rejection and the ServerDB token acquisition. Pure functions: no
// logging, no global state, no secrets (callers pass reasons built from HTTP status and curl codes,
// never from credentials; URLs go through nevr_log_diagnostics::RedactUrlForDiagnostics).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "runtime/log/url_diagnostics.h"

namespace FailureDetail {

// How many payload bytes DescribeRegistrationRejection quotes.
inline constexpr size_t kPayloadPreviewBytes = 64;

// Names the code byte of SNSLobbyRegistrationFailure. The server sends exactly one byte, a
// BroadcasterRegistrationFailureCode (nakama server/evr/broadcaster_registration_failure.go,
// values 0-11 in declaration order).
inline const char* RegistrationFailureCodeName(unsigned char code) {
  static constexpr const char* kNames[] = {
      "InvalidRequest", "Timeout",       "CryptographyError", "DatabaseError", "AccountDoesNotExist", "ConnectionFailed",
      "ConnectionLost", "ProviderError", "Restricted",        "Unknown",       "Failure",             "Success"};
  return code < sizeof(kNames) / sizeof(kNames[0]) ? kNames[code] : "UnrecognizedCode";
}

// A one-byte payload is the server's code byte: report it as `code=<n> (<name>)`. Any other shape is
// not what the server sends, so report the byte count and the first kPayloadPreviewBytes bytes,
// printable ASCII as-is and everything else as '.'.
inline std::string DescribeRegistrationRejection(const void* payload, uint64_t size) {
  std::string out = "payload_bytes=" + std::to_string(size);
  if (payload == nullptr || size == 0) return out + " payload_preview=\"\"";
  const auto* bytes = static_cast<const unsigned char*>(payload);
  if (size == 1) {
    return out + " code=" + std::to_string(bytes[0]) + " (" + RegistrationFailureCodeName(bytes[0]) + ")";
  }
  const size_t shown = size < kPayloadPreviewBytes ? static_cast<size_t>(size) : kPayloadPreviewBytes;
  out += " payload_preview=\"";
  for (size_t i = 0; i < shown; ++i) {
    const unsigned char c = bytes[i];
    out += (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? static_cast<char>(c) : '.';
  }
  out += '"';
  if (shown < size) out += " (truncated)";
  return out;
}

// Password-auth failure reasons. The URL is operator config and may carry userinfo or a query token,
// so it is always redacted.
inline std::string PasswordAuthRequestFailed(std::string_view httpUri, std::string_view curlMessage, int curlCode) {
  return "password auth: request to " + nevr_log_diagnostics::RedactUrlForDiagnostics(httpUri) + " failed: " +
         std::string(curlMessage) + " (curl code " + std::to_string(curlCode) + ")";
}

inline std::string PasswordAuthHttpStatus(std::string_view httpUri, long httpCode) {
  return "password auth: " + nevr_log_diagnostics::RedactUrlForDiagnostics(httpUri) + " answered HTTP " +
         std::to_string(httpCode);
}

// Pulls the access token out of the password-auth response. A body that is not a JSON object, or a
// `token` that is not a string, yields "" with a reason naming which; neither throws.
inline std::string ExtractAuthToken(std::string_view response, std::string& reason) {
  const nlohmann::json j = nlohmann::json::parse(response.begin(), response.end(), nullptr, false);
  if (j.is_discarded()) {
    reason = "password auth: the response was not valid JSON";
    return "";
  }
  if (!j.is_object()) {
    reason = "password auth: the response was not a JSON object";
    return "";
  }
  const auto it = j.find("token");
  if (it == j.end() || (it->is_string() && it->get_ref<const std::string&>().empty())) {
    reason = "password auth: the response carried no token";
    return "";
  }
  if (!it->is_string()) {
    reason = "password auth: the response token was not a string";
    return "";
  }
  return it->get<std::string>();
}

// Appends the cause to the base message. An empty reason leaves the base message unchanged.
inline std::string WithCause(std::string_view base, std::string_view reason) {
  std::string out(base);
  if (!reason.empty()) {
    out += ": ";
    out += reason;
  }
  return out;
}

}  // namespace FailureDetail
