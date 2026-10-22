/* The libcurl Transport for the data fetch (docs/adr/0011-quest-game-data-first-run.md). Same trust rules as
 * quest/auth/curl_http.h: certificates read from the Android CA directories into memory and handed to libcurl as
 * CAINFO_BLOB, peer and host verification always on, no proxy from the environment, redirects not followed. With no
 * certificate loaded every request fails closed with kNoTrustAnchors. HTTPS only; allow_plain_http exists for the
 * loopback tests and production passes false.
 *
 * There is no whole-transfer timeout (the body is 937 MB): a connection that never opens ends after
 * `connect_timeout_seconds`, and one that moves under 1 KiB/s for `stall_seconds` ends as a transport failure the
 * fetch retries from the part's size.
 */
#pragma once

#include <string>
#include <vector>

#include "core/auth_types.h"
#include "quest/auth/ca_bundle.h"
#include "quest/data/transport.h"

namespace nevr_quest::data {

class CurlTransport : public Transport {
 public:
  static constexpr int kNoTrustAnchors = -1;

  explicit CurlTransport(std::vector<std::string> ca_dirs = nevr::quest_auth::AndroidCaDirs(),
                         nevr::auth::LogSink log = nullptr, bool allow_plain_http = false,
                         long connect_timeout_seconds = 15, long stall_seconds = 30);

  TransportResult GetRange(const std::string& url, std::uint64_t offset, const BodyFn& on_body,
                           const KeepGoingFn& keep_going) override;

 private:
  std::vector<std::string> ca_dirs_;
  nevr::auth::LogSink log_;
  bool allow_plain_http_;
  long connect_timeout_seconds_;
  long stall_seconds_;
};

}  // namespace nevr_quest::data
