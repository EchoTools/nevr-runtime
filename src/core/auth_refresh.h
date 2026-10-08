#pragma once
// Access-token refresh against the device/auth/refresh RPC, split from transport
// and from persistence so the Windows runtime and the Quest shim share it.
//
// Failure contract: any outcome other than Refreshed leaves `auth` byte-for-byte
// unchanged. The caller persists only on Refreshed, so a failed refresh (timeout,
// 5xx, malformed body) can never overwrite or discard the cached login.

#include "core/auth_token_model.h"
#include "core/auth_types.h"

#include <cstdint>
#include <string>

namespace nevr::auth {

// How long before expiry the background refresh runs. The Windows module's
// comment says this must stay BELOW kFallbackAccessTokenLifetimeSec
// (core/auth_token_model.h) because at equal values a token with neither `exp` nor
// `expires_in` satisfies the guard the instant it is issued. Both are 300 today, so
// that stated margin does not exist; the value is kept (this extraction changes no
// behaviour) and the assert below only forbids the lead exceeding the fallback.
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
  Rejected,     // HTTP status other than 200
  Malformed,    // body is not the JSON object the RPC returns
  NoAccessToken // 200 with neither access_token nor token
};

const char* RefreshOutcomeName(RefreshOutcome outcome);

// "<base>/v2/rpc/device/auth/<endpoint>?http_key=<key>&unwrap". Nakama wraps an
// RPC response as {"payload":"<json string>"} unless &unwrap is present.
std::string BuildDeviceAuthUrl(const std::string& base_url, const std::string& http_key,
                               const std::string& endpoint);

std::string BuildRefreshBody(const std::string& refresh_token);

// Interprets the RPC's HTTP result. Mutates `auth` only when returning Refreshed.
RefreshOutcome ApplyRefreshResponse(CachedAuthToken& auth, const HttpResponse& response, uint64_t now,
                                    const LogSink& log);

// Build request, send, interpret. Does not persist.
RefreshOutcome RefreshAccessToken(CachedAuthToken& auth, const std::string& base_url,
                                  const std::string& http_key, HttpClient& http, uint64_t now,
                                  const LogSink& log);

}  // namespace nevr::auth
