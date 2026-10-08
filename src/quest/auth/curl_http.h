#pragma once
// libcurl implementation of nevr::auth::HttpClient for the Quest shim. Android's NDK
// ships no HTTP/TLS client, so this links the vcpkg `curl` (OpenSSL) built for
// arm64-android (src/quest/vcpkg.json).
//
// Trust: the certificates are read from the Android CA directories into memory
// (ca_bundle.h) and handed to libcurl as CAINFO_BLOB. Peer and host verification are
// always on; with no certificate loaded every request fails closed with
// kNoTrustAnchors. Proxy environment variables are ignored, redirects are not followed,
// and a response is capped at max_response_bytes.

#include "core/auth_types.h"
#include "quest/auth/ca_bundle.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace nevr::quest_auth {

class CurlHttpClient : public nevr::auth::HttpClient {
 public:
  // transport_code values that are not CURLcode values.
  static constexpr int kNoTrustAnchors = -1;
  static constexpr int kInterrupted = -2;

  // allow_plain_http exists for the loopback adapter tests only; production passes false.
  explicit CurlHttpClient(std::vector<std::string> ca_dirs = AndroidCaDirs(), nevr::auth::LogSink log = nullptr,
                          long timeout_seconds = 10, bool allow_plain_http = false,
                          size_t max_response_bytes = 1024 * 1024);
  nevr::auth::HttpResponse PostJson(const std::string& url, const std::string& body) override;
  void Interrupt() override;

 private:
  const CaBundle& Bundle();

  std::vector<std::string> ca_dirs_;
  nevr::auth::LogSink log_;
  long timeout_seconds_;
  bool allow_plain_http_;
  size_t max_response_bytes_;
  std::atomic<bool> interrupted_{false};
  std::mutex bundle_mutex_;
  bool bundle_loaded_ = false;
  CaBundle bundle_;
};

}  // namespace nevr::quest_auth
