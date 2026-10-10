#pragma once

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

}  // namespace nevr::lifecycle
