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

// The game's default API connection (https://api.readyatdawn.com, and the https://api-<env> form) when no
// apiservice_host / loginservice_host / api_host override applied: it goes to nevr_http_uri, the service
// that answers the game's REST calls (#408). Returns `uri` itself (same pointer) for anything else, before
// the redirects are armed, and when no http target is configured.
template <typename GetHttpTarget, typename Redirect>
const char* DecideUnconfiguredApiRedirect(bool armed, const char* uri, GetHttpTarget getHttpTarget,
                                          Redirect redirect) {
  if (uri == nullptr || std::strncmp(uri, "https://api", 11) != 0) return uri;
  return DecideServiceRedirect(armed, "apiservice_host", uri, getHttpTarget, redirect);
}

}  // namespace nevr::lifecycle
