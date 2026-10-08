#include "core/auth_refresh.h"

#include <nlohmann/json.hpp>

#include <cctype>

namespace nevr::auth {

namespace {
void Emit(const LogSink& log, LogLevel level, const std::string& message) {
  if (log) log(level, message);
}
}  // namespace

const char* RefreshOutcomeName(RefreshOutcome outcome) {
  switch (outcome) {
    case RefreshOutcome::Refreshed: return "refreshed";
    case RefreshOutcome::NoRefreshToken: return "no_refresh_token";
    case RefreshOutcome::TransportFailed: return "transport_failed";
    case RefreshOutcome::Denied: return "denied";
    case RefreshOutcome::Unauthorized: return "unauthorized";
    case RefreshOutcome::Rejected: return "rejected";
    case RefreshOutcome::Malformed: return "malformed";
    case RefreshOutcome::NoAccessToken: return "no_access_token";
  }
  return "unknown";
}

std::string BuildDeviceAuthUrl(const std::string& base_url, const std::string& http_key,
                               const std::string& endpoint) {
  return base_url + "/v2/rpc/device/auth/" + endpoint + "?http_key=" + http_key + "&unwrap";
}

std::string BuildRefreshBody(const std::string& refresh_token) {
  nlohmann::json body;
  // RFC 6749 §6 names this field `refresh_token`, and the RPC prefers it
  // (EchoTools/nakama f945f631d). Both are sent with the SAME value because the
  // two ends deploy on different days: a nakama older than that commit reads
  // only `token` and would reject a refresh_token-only body with
  // "invalid payload: token required".
  //
  // TEMPORARY. Delete the `token` line once no nakama older than f945f631d is
  // deployed -- the new server ignores it whenever refresh_token is present.
  body["refresh_token"] = refresh_token;
  body["token"] = refresh_token;  // deprecated: pre-RFC field name
  return body.dump();
}

RefreshOutcome ApplyRefreshResponse(CachedAuthToken& auth, const HttpResponse& response, uint64_t now,
                                    const LogSink& log) {
  if (!response.transport_ok) {
    Emit(log, LogLevel::Warning,
         "[NEVR.AUTH] token refresh request failed curl_code=" + std::to_string(response.transport_code) +
             " \xE2\x80\x94 falling back to cached/password auth");
    return RefreshOutcome::TransportFailed;
  }
  if (response.status != 200) {
    std::string lower = response.body;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool names_token = lower.find("refresh token") != std::string::npos ||
                             lower.find("refresh_token") != std::string::npos;
    Emit(log, LogLevel::Warning,
         "[NEVR.AUTH] token refresh rejected http_status=" + std::to_string(response.status) +
             " response_bytes=" + std::to_string(response.body.size()) +
             " names_refresh_token=" + (names_token ? "1" : "0"));
    const bool client_error = response.status == 400 || response.status == 401 || response.status == 403;
    if (client_error && names_token) return RefreshOutcome::Denied;
    if (response.status == 401 || response.status == 403) return RefreshOutcome::Unauthorized;
    return RefreshOutcome::Rejected;
  }

  try {
    const auto j = nlohmann::json::parse(response.body);

    // RFC 6749 §5.1 `access_token`, falling back to the deprecated `token`. The
    // fallback is required, not defensive: a nakama older than
    // EchoTools/nakama f945f631d returns only `token`.
    std::string new_token = j.value("access_token", "");
    if (new_token.empty()) new_token = j.value("token", "");
    const std::string new_refresh = j.value("refresh_token", "");

    if (new_token.empty()) {
      Emit(log, LogLevel::Warning,
           "[NEVR.AUTH] token refresh response carried no access_token or token field \xE2\x80\x94 treating as "
           "failed refresh");
      return RefreshOutcome::NoAccessToken;
    }

    // Same authority order as the device-poll path (ResolveAccessTokenExpirySec)
    // so the two ways of obtaining an access token cannot disagree about when
    // it dies.
    const uint64_t token_expiry =
        ResolveAccessTokenExpirySec(now, new_token, ReadExpiresInSeconds(j, "expires_in"));
    const uint64_t refresh_expiry =
        new_refresh.empty() ? 0
                            : ResolveRefreshTokenExpirySec(
                                  now, ReadExpiresInSeconds(j, "refresh_token_expires_in"));

    // Everything parsed: only now touch `auth`.
    auth.token = new_token;
    auth.token_expiry = token_expiry;
    if (!new_refresh.empty()) {
      auth.refresh_token = new_refresh;
      auth.refresh_token_expiry = refresh_expiry;
    }
    return RefreshOutcome::Refreshed;
  } catch (const nlohmann::json::parse_error&) {
    Emit(log, LogLevel::Warning,
         "[NEVR.AUTH] token refresh response was not valid JSON \xE2\x80\x94 treating as failed refresh");
    return RefreshOutcome::Malformed;
  } catch (const nlohmann::json::exception&) {
    // Valid JSON of the wrong shape ([] or {"access_token":1}): value() throws type_error.
    Emit(log, LogLevel::Warning,
         "[NEVR.AUTH] token refresh response was valid JSON of an unexpected shape \xE2\x80\x94 treating as "
         "failed refresh");
    return RefreshOutcome::Malformed;
  }
}

RefreshOutcome RefreshAccessToken(CachedAuthToken& auth, const std::string& base_url,
                                  const std::string& http_key, HttpClient& http, uint64_t now,
                                  const LogSink& log) {
  if (auth.refresh_token.empty()) return RefreshOutcome::NoRefreshToken;
  const HttpResponse response =
      http.PostJson(BuildDeviceAuthUrl(base_url, http_key, "refresh"), BuildRefreshBody(auth.refresh_token));
  return ApplyRefreshResponse(auth, response, now, log);
}

}  // namespace nevr::auth
