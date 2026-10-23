/* The seam between the fetch state machine (fetch.h) and the network (docs/adr/0011-quest-game-data-first-run.md).
 *
 * A Transport performs one ranged GET and hands the body to a callback; it never touches the filesystem and never
 * decides what a status means. The libcurl one is curl_transport.h; the tests use a scripted one.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nevr_quest::data {

struct TransportResult {
  bool completed = false;  // the transfer ran to its end (a status was received and the body, if any, was delivered)
  long http_status = 0;    // 0 when no response was received
  int code = 0;            // a transport-specific failure code when !completed, for the log
  bool interrupted = false;  // the transfer was ended because `keep_going` said stop
  std::string detail;      // a short reason for the log, never a URL
};

// Delivers body bytes with the response status they came with. Returns false to end the transfer (a failed write).
using BodyFn = std::function<bool(long http_status, const void* data, std::size_t length)>;
// Called at a coarse interval during the transfer; returns false to end it (a cancel).
using KeepGoingFn = std::function<bool()>;

class Transport {
 public:
  virtual ~Transport() = default;
  // GET `url` with the header `Range: bytes=<offset>-`. A body is delivered through `on_body` as it arrives.
  virtual TransportResult GetRange(const std::string& url, std::uint64_t offset, const BodyFn& on_body,
                                   const KeepGoingFn& keep_going) = 0;
};

}  // namespace nevr_quest::data
