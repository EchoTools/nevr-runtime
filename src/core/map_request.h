/* The request entry of the custom-map channel (docs/adr/0048-custom-maps-channel.md, decisions 9, 13 and 16): what a session needs
 * reaches the channel's worker here, and nowhere else. A client's lobby session success asks for its group (RequestGroup: the group
 * becomes the session's, and only the maps the cache already holds at an older version are fetched); the session pin asks for one
 * map of that group when the client lacks it or holds another version (RequestMap). A server asks for its whole group.
 *
 * A client's fetch is on demand (FetchScope): a bucket of many maps costs a player only the maps it plays. The worker runs one group
 * at a time (GroupFetchQueue); each fetch takes the union of the ids asked for that group since the last one. After the fetch the
 * channel mounts what arrived (Channel::ServeRequests), so a map fetched mid-run is played without a restart.
 *
 * Every answer is one `map_request` record through the Env, with the consequence in plain words.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "cdn/cdn_env.h"
#include "core/map_fetch_queue.h"
#include "core/map_run.h"

namespace nevr_maps {

// The plain words for a request's answer (the `consequence` of its record): what the player gets, or why not. `named_map`: the
// request named a map (the session pin's), not only a group. `mounts_now` false: what arrives plays after the next start
// (ChannelConfig::mounts_mid_run).
std::string RequestConsequence(const FetchReport& report, bool named_map, bool is_server, bool mounts_now = true);

class MapRequests {
 public:
  using Fetch = std::function<FetchReport(const std::string& group, const FetchScope& scope)>;

  // `env` takes the records; it must outlive this object. A server fetches every map its group lists, a client on demand.
  // `mounts_now` is the channel's ChannelConfig::mounts_mid_run: the records say when what arrived is played.
  MapRequests(nevr_cdn::Env& env, bool is_server, bool mounts_now = true)
      : env_(env), is_server_(is_server), mounts_now_(mounts_now) {}
  MapRequests(const MapRequests&) = delete;
  MapRequests& operator=(const MapRequests&) = delete;

  // Any thread. A client's session success (and a server's allocation): the group's manifest is fetched, with the maps the cache
  // holds at an older version. On a client the group becomes the session's: RequestMap asks it. False, with `reason` set (bad_group,
  // stopped) and one record, when refused.
  bool RequestGroup(const std::string& group, std::string* reason);

  // Any thread. The session needs map `id` of the session's group: the map is fetched with the rest of the group's scope. False,
  // with `reason` set (no_session_group, bad_id, stopped) and one record, when refused.
  bool RequestMap(const std::string& id, std::string* reason);

  // The worker. Runs the oldest waiting group through `fetch` with the scope asked for it. False when none was waiting.
  bool RunNext(const Fetch& fetch);
  bool WaitForRequest(std::uint32_t timeout_ms) { return queue_.WaitForRequest(timeout_ms); }

  // Any thread. Later requests are refused; every waiting one is answered failed/cancelled now.
  void Stop() {
    stopped_.store(true);
    queue_.Stop();
  }
  bool stopped() const { return stopped_.load(); }

  std::string SessionGroup() const;
  std::size_t Waiting() const { return queue_.Waiting(); }

 private:
  bool Queue(const std::string& group, const std::string& id, std::string* reason);
  void Refused(const std::string& group, const std::string& id, const std::string& reason);

  nevr_cdn::Env& env_;
  const bool is_server_;
  const bool mounts_now_;
  GroupFetchQueue queue_;
  std::atomic<bool> stopped_{false};
  mutable std::mutex mutex_;
  std::string session_group_;
  std::map<std::string, std::set<std::string>> ids_;  // group -> the ids asked since its last fetch began
};

// One process's request entry, for the seams that take a plain function (nevr_maps::SetSessionGroupRequest,
// nevr_map_session_pin::SetFetchRequest): Install sets both to this entry; null clears them. The entry must outlive every call.
void InstallRequestSeams(MapRequests* requests);
// The installed entry's RequestGroup for a seam that answers whether the request was queued (the server's maps hold). False when
// none is installed.
bool RequestGroupThroughSeam(const std::string& group);

}  // namespace nevr_maps
