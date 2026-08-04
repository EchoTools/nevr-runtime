#include "device_poll_response.h"

#include "core/auth_token.h"

#include <nlohmann/json.hpp>

namespace TokenAuth {

DevicePollResponse ParseDevicePollResponse(std::string_view response) {
  DevicePollResponse result;
  try {
    const nlohmann::json json = nlohmann::json::parse(response);
    if (json.contains("error")) return result;

    const std::string status = json.value("status", "");
    if (status == "expired") {
      result.status = DevicePollStatus::Expired;
      return result;
    }
    if (status != "verified") {
      result.status = DevicePollStatus::Pending;
      return result;
    }

    result.access_token = json.value("token", "");
    if (result.access_token.empty()) return result;
    result.status = DevicePollStatus::Verified;
    result.refresh_token = json.value("refresh_token", "");
    result.user_id = json.value("user_id", "");
    result.username = json.value("username", "");
    if (json.contains("expires_in") && json["expires_in"].is_number_unsigned()) {
      result.expires_in = json["expires_in"].get<uint64_t>();
    }
    return result;
  } catch (const nlohmann::json::exception&) {
    return result;
  }
}

uint64_t ResolveAccessTokenExpiry(uint64_t now, const std::string& access_token,
                                  std::optional<uint64_t> expires_in) {
  CachedAuthToken token;
  token.token = access_token;
  const uint64_t jwtExpiry = token.GetJwtExpiry();
  if (jwtExpiry > now) return jwtExpiry;
  if (expires_in.has_value()) return now + *expires_in;
  return now + kFallbackAccessTokenLifetimeSec;
}

}  // namespace TokenAuth
