#pragma once

// Operator-facing cause text for the two ServerFatal sites in gameserver.cpp that used to say only
// "failed" (#35): the registration rejection and the ServerDB token acquisition. Pure functions: no
// logging, no global state, no secrets (callers pass reasons built from HTTP status and curl codes,
// never from credentials).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace FailureDetail {

// How many payload bytes DescribeRegistrationRejection quotes.
inline constexpr size_t kPayloadPreviewBytes = 64;

// The SNSLobbyRegistrationFailure payload has no protobuf form (its layout is the legacy binary
// message), so no reason field can be decoded. Report what is certain: the byte count and the first
// kPayloadPreviewBytes bytes, printable ASCII as-is and everything else as '.', so an operator can
// see a text reason if the server sent one.
inline std::string DescribeRegistrationRejection(const void* payload, uint64_t size) {
  std::string out = "payload_bytes=" + std::to_string(size);
  if (payload == nullptr || size == 0) return out + " payload_preview=\"\"";
  const auto* bytes = static_cast<const unsigned char*>(payload);
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
