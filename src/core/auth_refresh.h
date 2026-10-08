#pragma once
// Access-token refresh against the device/auth/refresh RPC. Transport and persistence
// are the caller's, so the Windows runtime and the Quest shim share this one source.
//
// Failure contract: any outcome other than Refreshed leaves `auth` byte-for-byte
// unchanged. The caller persists only on Refreshed, so a failed refresh (timeout,
// 5xx, malformed body) can never overwrite or discard the cached login.

#include "core/auth_token_model.h"
#include "core/auth_types.h"

#include <cstdint>
#include <string>

namespace nevr::auth {

// How long before expiry the background refresh runs. It may not exceed
// kFallbackAccessTokenLifetimeSec (core/auth_token_model.h). At equality, which is
// today's state (both 300), a token carrying neither a decodable `exp` nor a server
// `expires_in` is due for refresh from the moment it is issued, so each background
// wake refreshes it. Production nakama always signs `exp`, so that case is not
// reached; raising the lead above the fallback is what the assert below rejects.
inline constexpr uint64_t kRefreshLeadSec = 300;

static_assert(kRefreshLeadSec <= kFallbackAccessTokenLifetimeSec,
              "refresh lead must not exceed the fallback access-token lifetime");

// True when the LIVE access token (expiry from memory; it is never persisted)
// is inside the lead window or already dead.
inline bool AccessTokenNeedsRefresh(uint64_t live_expiry, uint64_t now) {
  return live_expiry <= now + kRefreshLeadSec;
}

enum class RefreshOutcome {
  Refreshed,
  NoRefreshToken,
  TransportFailed,
  Denied,        // 400/401/403 whose JSON message is one of the refresh RPC's own errors: THIS token is refused
  Unauthorized,  // any other 401/403: a wrong http_key or a gateway, not the token
  ClientError,   // any other 4xx (400 "missing payload", 404 RPC not found, ...) except 408/429: permanent
  Rejected,      // 5xx, 408, 429 and any other non-200: transient
  Malformed,     // body is not the JSON object the RPC returns
  NoAccessToken  // 200 with neither access_token nor token
};

const char* RefreshOutcomeName(RefreshOutcome outcome);

// "<base>/v2/rpc/device/auth/<endpoint>?http_key=<key>&unwrap". Nakama wraps an
// RPC response as {"payload":"<json string>"} unless &unwrap is present.
std::string BuildDeviceAuthUrl(const std::string& base_url, const std::string& http_key,
                               const std::string& endpoint);

std::string BuildRefreshBody(const std::string& refresh_token);

// Interprets the RPC's HTTP result. Mutates `auth` only when returning Refreshed.
//
// A 401 is also what nakama answers for a wrong http_key (server/api_rpc.go), so the status
// alone cannot say the refresh token is bad. The body of a nakama error is JSON with "error",
// "message" and "code"; Denied needs a 400/401/403 whose "message" (or "error") is exactly one of
// the refresh RPC's own errors (server/evr_device_auth.go): "invalid or expired refresh token",
// "refresh token expired", "not a refresh token", "invalid payload: refresh_token required".
// A body that is not JSON, or that merely echoes the request, is never Denied.
RefreshOutcome ApplyRefreshResponse(CachedAuthToken& auth, const HttpResponse& response, uint64_t now,
                                    const LogSink& log);

// Build request, send, interpret. Does not persist.
RefreshOutcome RefreshAccessToken(CachedAuthToken& auth, const std::string& base_url,
                                  const std::string& http_key, HttpClient& http, uint64_t now,
                                  const LogSink& log);

}  // namespace nevr::auth
