/* The requests for a lobby group's maps (docs/adr/0048-custom-maps-channel.md): a client asks when a lobby session succeeds, and a
 * dedicated server when it is allocated a session, each with the session's group id. A request is answered on the channel's
 * worker, which runs the fetch (core/map_run.h, FetchGroup) one group at a time, never on the game's frame; the caller learns the
 * outcome from its callback: fetched, cached or failed with a reason.
 *
 * A request for a group that is already waiting joins it: the group is fetched once and every callback hears the one result. A
 * request for the group being fetched waits for the next fetch, because the running one may have read the manifest already. Stop
 * answers every waiting request with failed/cancelled, so a caller that holds something until it hears is never left holding.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/map_run.h"

namespace nevr_maps {

class GroupFetchQueue {
 public:
  using Done = std::function<void(const FetchReport&)>;
  using Fetch = std::function<FetchReport(const std::string& group)>;

  // Any thread. False, with `reason` set and the callback never called, when the id is not a path-safe group id (bad_group) or
  // the queue has stopped (stopped).
  bool Request(const std::string& group, Done done, std::string* reason);

  // The worker. Runs the oldest waiting group through `fetch` and calls each of its callbacks with the result, outside the lock.
  // False when no request was waiting.
  bool RunNext(const Fetch& fetch);

  // The worker. True as soon as a request waits, false when `timeout_ms` passed without one or the queue stopped.
  bool WaitForRequest(std::uint32_t timeout_ms);

  // Any thread. Later requests are refused, and every waiting one is answered failed/cancelled now.
  void Stop();

  std::size_t Waiting() const;

 private:
  struct Pending {
    std::string group;
    std::vector<Done> done;
  };

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Pending> pending_;
  bool stopped_ = false;
};

}  // namespace nevr_maps
