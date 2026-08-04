#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace TokenAuth {

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
};

DevicePollResponse ParseDevicePollResponse(std::string_view response);

// A freshly issued JWT's exp claim is the authority. expires_in is a fallback
// for tokens that omit exp, followed by the conservative runtime default.
uint64_t ResolveAccessTokenExpiry(uint64_t now, const std::string& access_token,
                                  std::optional<uint64_t> expires_in);

}  // namespace TokenAuth
