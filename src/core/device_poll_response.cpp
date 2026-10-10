#include "core/device_poll_response.h"

#include "core/auth_token_model.h"

#include <nlohmann/json.hpp>

namespace nevr_token_auth {

DevicePollResponse ParseDevicePollResponse(std::string_view response) {
  try {
    const nlohmann::json json = nlohmann::json::parse(response);
    DevicePollResponse result;
    result.answered = true;
    if (json.contains("error")) return result;

    const std::string status = json.value("status", "");
    result.server_status = status;
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
    result.refresh_token = json.value("refresh_token", "");
    result.user_id = json.value("user_id", "");
    result.username = json.value("username", "");
    // Both fields are RFC 6749 seconds-from-now. ReadExpiresInSeconds carries the
    // guard against the server's negative "already expired" values and keeps
    // absence absent; see core/auth_token.h.
    result.expires_in = ReadExpiresInSeconds(json, "expires_in");
    result.refresh_token_expires_in = ReadExpiresInSeconds(json, "refresh_token_expires_in");
    result.status = DevicePollStatus::Verified;
    return result;
  } catch (const nlohmann::json::exception&) {
    return DevicePollResponse{};
  }
}

bool IsKnownPollStatus(std::string_view status) {
  return status == "authorization_pending" || status == "pending" || status == "expired" || status == "verified";
}

std::string PollBodyPrefix(std::string_view body, std::string_view code, std::size_t max_chars) {
  // A body that holds tokens is never echoed; neither is one that merely contains a token-shaped field.
  for (const char* secret : {"access_token", "refresh_token", "\"token\""}) {
    if (body.find(secret) != std::string_view::npos) {
      return "<" + std::to_string(body.size()) + " bytes, token fields not logged>";
    }
  }
  std::string out;
  out.reserve(max_chars);
  for (std::size_t i = 0; i < body.size() && out.size() < max_chars;) {
    if (!code.empty() && body.compare(i, code.size(), code) == 0) {
      out += "<code>";
      i += code.size();
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(body[i]);
    out += (c < 0x20 || c == 0x7f) ? '.' : static_cast<char>(c);
    ++i;
  }
  return out;
}

uint64_t ResolveAccessTokenExpiry(uint64_t now, const std::string& access_token,
                                  std::optional<uint64_t> expires_in) {
  return ResolveAccessTokenExpirySec(now, access_token, expires_in);
}

}  // namespace nevr_token_auth
