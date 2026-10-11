#pragma once
// WsConnector over libcurl's WebSocket API (CURLOPT_CONNECT_ONLY=2), reusing the libcurl-over-OpenSSL the
// Quest vcpkg manifest provides (ADR 0003: no second TLS stack).
//
// Verification and routing are not configurable here. Peer and host verification are always on, the minimum
// protocol is TLS 1.2, the scheme is restricted to wss, redirects are off, and proxies are off
// (CURLOPT_NOPROXY "*": libcurl otherwise honours all_proxy, https_proxy, wss_proxy and friends from the
// environment and would send the Authorization header to whatever they name). There is no field, flag or
// environment variable that changes any of it. Trust is the certificates found in Config::caDirs (the Android CA
// directories by default), read into memory by the same loader token auth uses
// (quest/auth/ca_bundle.h) and handed to libcurl as CURLOPT_CAINFO_BLOB. CURLOPT_CAPATH is not used:
// OpenSSL looks certificates up by the SHA-1 subject hash and Android names them by the old MD5 hash,
// so a CApath on Android finds nothing. With no certificate loaded every Connect fails closed.

#include <mutex>
#include <string>
#include <vector>

#include "quest/auth/ca_bundle.h"

#include "quest/net/remote_ws.h"

namespace quest_net {

class CurlWsConnector final : public WsConnector {
 public:
  struct Config {
    std::vector<std::string> caDirs = nevr::quest_auth::AndroidCaDirs();  // first directory with any cert wins
    long connectTimeoutSeconds = 15;
    std::size_t maxMessageBytes = 4u * 1024u * 1024u;
    nevr_session_router::LogSink log;
  };

  explicit CurlWsConnector(Config config);
  ConnectResult Connect(const ConnectRequest& request) override;

  // ConnectResult::nativeCode when no trust anchor could be loaded (same value token auth uses).
  static constexpr int kNoTrustAnchors = -1;

 private:
  Config config_;
  std::mutex bundleMutex_;
  nevr::quest_auth::CaBundle bundle_;  // loaded on first use; a failed load is retried on the next Connect
};

// Maps a CURLcode (as int, so callers need not include curl.h) to the transport's status. Exposed for tests.
ConnectStatus ClassifyCurlCode(int curlCode, long httpStatus);

}  // namespace quest_net
