#pragma once
// libcurl implementation of nevr::auth::HttpClient for the Quest shim. Android's NDK
// ships no HTTP/TLS client, so this links the vcpkg `curl` (OpenSSL) built for
// arm64-android (src/quest/vcpkg.json). Certificate and host verification are always
// on; the system CA directory is passed explicitly because the vcpkg build's default
// CA path is that of the build host.

#include "core/auth_types.h"

#include <string>

namespace nevr::quest_auth {

inline constexpr char kAndroidSystemCaDir[] = "/system/etc/security/cacerts";

class CurlHttpClient : public nevr::auth::HttpClient {
 public:
  // allow_plain_http exists for the loopback adapter tests only; production passes false.
  explicit CurlHttpClient(std::string ca_dir = kAndroidSystemCaDir, long timeout_seconds = 10,
                          bool allow_plain_http = false);
  nevr::auth::HttpResponse PostJson(const std::string& url, const std::string& body) override;

 private:
  std::string ca_dir_;
  long timeout_seconds_;
  bool allow_plain_http_;
};

}  // namespace nevr::quest_auth
