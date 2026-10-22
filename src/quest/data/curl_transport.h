/* The libcurl Transport for the data fetch (docs/adr/0011-quest-game-data-first-run.md). The trust anchors are the PEM
 * the caller passes (production: data_host_roots.h, the roots of the CAs Cloudflare Universal SSL issues from), handed
 * to libcurl as CAINFO_BLOB; peer and host verification are always on, TLS 1.2 is the floor, no proxy from the
 * environment, redirects not followed. With an empty PEM every request fails closed with kNoTrustAnchors; with the
 * production roots, a table that no longer matches its pins fails closed with kRootsUnusable before any request.
 * HTTPS only; allow_plain_http exists for the loopback tests and production passes false.
 *
 * There is no whole-transfer timeout (the body is 937 MB): a connection that never opens ends after
 * `connect_timeout_seconds`, and one that moves under 1 KiB/s for `stall_seconds` ends as a transport failure the
 * fetch retries from the part's size.
 */
#pragma once

#include <functional>
#include <string>

#include "quest/data/data_host_roots.h"
#include "quest/data/transport.h"

namespace nevr_quest::data {

class CurlTransport : public Transport {
 public:
  static constexpr int kNoTrustAnchors = -1;
  static constexpr int kRootsUnusable = -2;

  /// `roots_check` is the self-check run before every request that uses the production roots; the default is
  /// CheckDataHostRoots (a test passes one that fails, since the production table is a constant).
  explicit CurlTransport(std::string roots_pem = DataHostRootsPem(), bool allow_plain_http = false,
                         long connect_timeout_seconds = 15, long stall_seconds = 30,
                         std::function<RootsCheck()> roots_check = CheckDataHostRoots);

  TransportResult GetRange(const std::string& url, std::uint64_t offset, const BodyFn& on_body,
                           const KeepGoingFn& keep_going) override;

 private:
  std::string roots_pem_;
  bool allow_plain_http_;
  long connect_timeout_seconds_;
  long stall_seconds_;
  std::function<RootsCheck()> roots_check_;
};

}  // namespace nevr_quest::data
