// The request entry of the custom-map channel's client (docs/adr/0048-custom-maps-channel.md, decisions 9 and 16): what a lobby
// session needs reaches the channel's worker here. A session success names its group (RequestGroup): the group becomes the
// session's group, and the worker refreshes the maps the cache holds of it at an older version. The session pin names one map of
// that group when the client lacks it or holds a version the channel no longer lists (RequestMap): the worker fetches that map.
//
// A client's fetch is on demand (FetchScope): only the ids asked for and the maps the cache already holds are fetched, so a bucket of
// many maps costs a player only the maps it plays. The worker runs one group at a time (GroupFetchQueue); each run takes the union of
// the ids asked for that group since its last run began. A fetched map mounts at the next launch (ADR 0048 decision 16).
//
// Every answer is one `map_request` record through the Env, with its consequence in plain words. No record carries a map file's
// contents or a key.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cdn/cdn_env.h"
#include "core/map_fetch_queue.h"
#include "core/map_run.h"

namespace nevr_maps {

// The consequence a request's answer states (the record's `consequence`). `named_map`: the request named a map, not only a group.
std::string RequestConsequence(const FetchReport& report, bool named_map);

class MapRequests {
 public:
  using Fetch = std::function<FetchReport(const std::string& group, const FetchScope& scope)>;
  // Called after a request is queued, on the thread that queued it: the PC glue wakes its worker so the request is run.
  using Wake = std::function<void()>;

  // `env` takes the records and must outlive this object.
  explicit MapRequests(nevr_cdn::Env& env) : env_(env) {}
  MapRequests(const MapRequests&) = delete;
  MapRequests& operator=(const MapRequests&) = delete;

  // Any thread. A session success names its group. The group becomes the session's (RequestMap asks within it), and the worker
  // fetches it on demand. False, with `reason` set (bad_group, stopped) and one record, when refused.
  bool RequestGroup(const std::string& group, std::string* reason);

  // Any thread. The session pin needs map `id` of the session's group. False, with `reason` set (no_session_group, bad_id, stopped)
  // and one record, when refused.
  bool RequestMap(const std::string& id, std::string* reason);

  // The worker. Runs the oldest waiting group through `fetch` with the scope its requests asked for. False when none was waiting.
  bool RunNext(const Fetch& fetch);
  bool WaitForRequest(std::uint32_t timeout_ms) { return queue_.WaitForRequest(timeout_ms); }

  void SetWake(Wake wake);
  // Called after a mount-only request is queued, on the thread that queued it: the edge wakes its worker's light job (FetchWorker::RequestLight),
  // which is not held by the gap, because a mount of a map already stored fetches nothing (ADR 0048 decision 16).
  void SetWakeLight(Wake wake);

  // The worker. The maps whose requests named a map the cache holds at the listed version (nevr_map_session_pin::CachedMapAwaitsMount),
  // by group: they are mounted at once, with no fetch (ServeMountOnly). Each is returned once.
  using MountOnly = std::vector<std::pair<std::string, std::vector<std::string>>>;
  MountOnly TakeMountOnly();

  // The worker. A map the mount-only path found deferred (a moved file stamp, a changed stock file) is asked for again as a fetching
  // request: its fetch hashes the deferred maps first (Channel::TakeDeferred), and its mount then grants the map. Under the gap, like any
  // fetching request.
  bool RetryAfterRehash(const std::string& group, const std::string& id, std::string* reason) { return Queue(group, id, reason); }

  // Any thread. Later requests are refused, and every waiting one is answered failed/cancelled now.
  void Stop();
  bool stopped() const { return stopped_.load(); }

  std::string SessionGroup() const;
  std::size_t Waiting() const { return queue_.Waiting(); }

 private:
  bool Queue(const std::string& group, const std::string& id, std::string* reason);
  bool QueueMountOnly(const std::string& group, const std::string& id, std::string* reason);
  void Refused(const std::string& group, const std::string& id, const std::string& reason);
  void WakeWorker();

  nevr_cdn::Env& env_;
  GroupFetchQueue queue_;
  std::atomic<bool> stopped_{false};
  mutable std::mutex mutex_;  // guards session_group_, ids_ and wake_
  std::string session_group_;
  std::map<std::string, std::set<std::string>> ids_;  // group -> the ids asked for since its last run began
  Wake wake_, wake_light_;
  std::map<std::string, std::set<std::string>> mount_only_;  // group -> the ids to mount with no fetch
};

// Installs one process's request entry on the two seams a session uses: the session success's group request
// (nevr_maps::SetSessionGroupRequest) and the session pin's map request (nevr_map_session_pin::SetFetchRequest). Null clears both.
// The entry must outlive every call made while it is installed.
void InstallRequestSeams(MapRequests* requests);

}  // namespace nevr_maps
