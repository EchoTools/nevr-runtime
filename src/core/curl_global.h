#pragma once

// libcurl's global state must be initialised before any handle is created, exactly once, and
// must outlive every thread that uses curl. Several subsystems (token auth, the
// asset CDN, the game server link) each create handles from their own threads; when one of
// them called curl_global_init/curl_global_cleanup around its own lifetime it tore the state
// down under the others (CDN requests failing with CURLE_UNSUPPORTED_PROTOCOL right after a
// good token refresh on Windows). Every curl user calls EnsureCurlGlobalInit() first, and
// nothing calls curl_global_cleanup: the process exit releases it.

#include <curl/curl.h>

#include <mutex>

namespace nevr {

inline void EnsureCurlGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

}  // namespace nevr
