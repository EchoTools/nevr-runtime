#pragma once
// #1156 (PR-D slice D1b), docs/adr/0017: the libcurl Transport the invite worker posts through (invite_worker.h).
//
// One POST of a JSON body to <base>/v2/rpc/<rpc>?unwrap with `Authorization: Bearer <token>` (the address is decided by
// invite_transport.h: https, or plain http to the machine itself). TLS is verified, a redirect is never followed (the token
// would go with it), the connection and the whole request have timeouts, and an answer larger than the cap is no answer. Any
// failure is status 0 and one `[NEVR.INVITE] transport ...` line that names the cause and never the token, the body or the
// address. Called from the worker thread only; Abort() may be called from any thread and makes a request in flight, and every
// later one, return at once, so a shutdown does not wait out a timeout.
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "runtime/patch/invite_worker.h"

namespace nevr_invite {

struct TransportOptions {
  std::function<void(LogLevel, const std::string&)> log;
  long connect_timeout_s = 5;
  long timeout_s = 10;
  std::size_t max_body_bytes = 64 * 1024;
};

class CurlTransport final : public Transport {
 public:
  CurlTransport(std::string base_url, TransportOptions options);
  HttpAnswer Post(std::string_view rpc, std::string_view body, std::string_view bearer) override;
  void Abort() override { aborted_.store(true, std::memory_order_release); }

 private:
  void Say(LogLevel level, const std::string& line) const {
    if (options_.log) options_.log(level, line);
  }

  std::string base_;
  TransportOptions options_;
  std::atomic<bool> aborted_{false};
};

}  // namespace nevr_invite
