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

    // RFC 6749 §5.1 names this `access_token`; the RPC originally called it
    // `token` and still returns that, deprecated (EchoTools/nakama f945f631d).
    // The fallback is load-bearing, not politeness: a nakama older than that
    // commit sends ONLY `token`, so an access_token-only reader authenticates
    // against nothing on every currently-deployed server.
    result.access_token = json.value("access_token", "");
    if (result.access_token.empty()) result.access_token = json.value("token", "");
    if (result.access_token.empty()) return result;
    result.status = DevicePollStatus::Verified;
    result.refresh_token = json.value("refresh_token", "");
    result.user_id = json.value("user_id", "");
    result.username = json.value("username", "");
    // Both fields are RFC 6749 seconds-from-now. ReadExpiresInSeconds carries the
    // guard against the server's negative "already expired" values and keeps
    // absence absent; see core/auth_token.h.
    result.expires_in = ReadExpiresInSeconds(json, "expires_in");
    result.refresh_token_expires_in = ReadExpiresInSeconds(json, "refresh_token_expires_in");
    return result;
  } catch (const nlohmann::json::exception&) {
    return result;
  }
}

uint64_t ResolveAccessTokenExpiry(uint64_t now, const std::string& access_token,
                                  std::optional<uint64_t> expires_in) {
  return ResolveAccessTokenExpirySec(now, access_token, expires_in);
}

}  // namespace TokenAuth
