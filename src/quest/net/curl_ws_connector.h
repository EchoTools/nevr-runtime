#pragma once
// WsConnector over libcurl's WebSocket API (CURLOPT_CONNECT_ONLY=2), reusing the libcurl-over-OpenSSL the
// Quest vcpkg manifest provides (ADR 0003: no second TLS stack).
//
// Verification is not configurable here. Peer and host verification are always on, the minimum protocol is
// TLS 1.2, the scheme is restricted to wss, redirects are off, and there is no field, flag or environment
// variable that relaxes any of it. Trust comes from Config::caDir (Android's system store by default) or,
// for tests that run their own certificate authority, Config::caFile.

#include <string>

#include "quest/net/remote_ws.h"

namespace quest_net {

class CurlWsConnector final : public WsConnector {
 public:
  struct Config {
    std::string caDir = "/system/etc/security/cacerts";  // Android's system CA store
    std::string caFile;                                  // optional extra trust anchor file (tests)
    long connectTimeoutSeconds = 15;
    std::size_t maxMessageBytes = 4u * 1024u * 1024u;
    SessionRouter::LogSink log;
  };

  explicit CurlWsConnector(Config config);
  ConnectResult Connect(const ConnectRequest& request) override;

 private:
  Config config_;
};

// Maps a CURLcode (as int, so callers need not include curl.h) to the transport's status. Exposed for tests.
ConnectStatus ClassifyCurlCode(int curlCode, long httpStatus);

}  // namespace quest_net
