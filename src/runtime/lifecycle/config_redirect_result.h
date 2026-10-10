#pragma once

#include <cstring>

namespace nevr::lifecycle {

// Keep the game's exact result pointer when no redirect applies. The game uses
// pointer identity with defaultValue to recognize a missing JSON key.
inline const char* ChooseRedirectedOrOriginal(const char* originalResult,
                                               const char* redirectedResult) noexcept {
  return redirectedResult == nullptr ? originalResult : redirectedResult;
}

// The decision inside the JsonValueAsString redirect. Returns `result` itself (the same pointer)
// unless a redirect applies. Nothing is looked up before the redirects are armed: the first lookup
// is the first access to the lazily loaded config.yaml singleton, which must not happen before
// the runtime bootstrap. `getHttpTarget()` returns the configured nevr_http_uri (or null) and
// `redirect(result, httpTarget)` the replacement (or null); both are invoked only when
// `result` and `keyName` are non-null and `armed` is true.
template <typename GetHttpTarget, typename Redirect>
const char* DecideServiceRedirect(bool armed, const char* keyName, const char* result,
                                  GetHttpTarget getHttpTarget, Redirect redirect) {
  if (result == nullptr || keyName == nullptr) return result;
  if (!armed) return result;
  const char* httpTarget = getHttpTarget();
  return ChooseRedirectedOrOriginal(result, redirect(result, httpTarget));
}

// The game's API base URL: "https://api.<host>" (api.readyatdawn.com) or "https://api-<env>.<host>"
// (api-%s.readyatdawn.com, libr15 0x126c240). Exact prefixes, so "https://apiary.example" is not one.
inline bool IsGameApiBaseUrl(const char* uri) {
  return uri != nullptr &&
         (std::strncmp(uri, "https://api.", 12) == 0 || std::strncmp(uri, "https://api-", 12) == 0);
}

// The game's default API connection when no apiservice_host / loginservice_host / api_host override
// applied: it goes to nevr_http_uri, the service that answers the game's REST calls (#408). Returns `uri`
// itself (same pointer) for anything else, before the redirects are armed, and when no http target is
// configured.
template <typename GetHttpTarget, typename Redirect>
const char* DecideUnconfiguredApiRedirect(bool armed, const char* uri, GetHttpTarget getHttpTarget,
                                          Redirect redirect) {
  if (!IsGameApiBaseUrl(uri)) return uri;
  return DecideServiceRedirect(armed, "apiservice_host", uri, getHttpTarget, redirect);
}

// The decision for one HttpConnect: `gameUri` is what the game passed, `afterConfig` what the configured
// service-host chain (apiservice_host, loginservice_host, api_host, ...) made of it. A configured host
// always wins: only when the chain left the game's own pointer is the unconfigured API fallback considered.
template <typename GetHttpTarget, typename Redirect>
const char* DecideHttpConnectUri(bool armed, const char* gameUri, const char* afterConfig,
                                 GetHttpTarget getHttpTarget, Redirect redirect) {
  if (afterConfig != gameUri) return afterConfig;
  return DecideUnconfiguredApiRedirect(armed, afterConfig, getHttpTarget, redirect);
}

}  // namespace nevr::lifecycle
