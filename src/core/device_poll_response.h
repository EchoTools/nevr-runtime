#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nevr_token_auth {

enum class DevicePollStatus {
  Pending,
  Verified,
  Expired,
  Error,
};

struct DevicePollResponse {
  DevicePollStatus status = DevicePollStatus::Error;
  std::string access_token;
  std::string refresh_token;
  std::string user_id;
  std::string username;
  std::optional<uint64_t> expires_in;
  // Absent when the server did not say. Empty is the honest answer here — a
  // caller must reach for the documented fallback rather than read a number
  // this side invented.
  std::optional<uint64_t> refresh_token_expires_in;
  // What the poll itself looked like, for the log (#397). `server_status` is the body's "status" string
  // as sent ("" when absent or the body was not JSON). `http_code` is 0 when no HTTP answer arrived (or
  // the caller is not an HTTP one). `body_prefix` is PollBodyPrefix of the body: never a token or the code.
  std::string server_status;
  long http_code = 0;
  std::string body_prefix;
};

/// Whether `status` is one the poll endpoint is known to send ("authorization_pending", "pending",
/// "expired", "verified"). Anything else is treated as pending and reported.
bool IsKnownPollStatus(std::string_view status);

/// At most `max_chars` of a poll body for a log line: every occurrence of `code` replaced by "<code>",
/// control characters shown as '.'. A body that carries tokens (a verified answer) is not echoed at all:
/// it is described by its size.
std::string PollBodyPrefix(std::string_view body, std::string_view code, std::size_t max_chars = 120);

DevicePollResponse ParseDevicePollResponse(std::string_view response);

// A freshly issued JWT's exp claim is the authority. expires_in is a fallback
// for tokens that omit exp, followed by the conservative runtime default.
// Thin forwarder to ResolveAccessTokenExpirySec in core/auth_token_model.h, which is
// the single definition — the refresh path (auth_token_refresh.h) needs the same
// ordering and cannot reach this translation unit.
uint64_t ResolveAccessTokenExpiry(uint64_t now, const std::string& access_token,
                                  std::optional<uint64_t> expires_in);

}  // namespace nevr_token_auth
