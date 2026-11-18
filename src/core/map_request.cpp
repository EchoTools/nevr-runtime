#include "core/map_request.h"

#include <atomic>

#include "core/map_groups.h"
#include "core/map_manifest.h"
#include "core/map_session_pin.h"

namespace nevr_maps {

std::string RequestConsequence(const FetchReport& report, bool named_map) {
  const std::string& reason = report.reason;
  if (report.outcome != FetchOutcome::kFailed) {
    if (report.maps_updated > 0) return "the map is stored and plays after your next launch";
    if (!named_map) return "the maps you hold of this group are the current versions";
    return "your copy is the current version";
  }
  if (reason == "not_listed") {
    return "this map is not in your lobby group's map list: a player pulls only the maps its group offers, and an old version is never "
           "fetched, so it cannot be played";
  }
  if (reason == "cancelled" || reason == "stopped") return "the game is closing: nothing more is fetched";
  if (reason == "no_session_group") return "no lobby group is known for this session, so the map cannot be asked for";
  if (reason == "bad_group" || reason == "bad_id") return "the session named an id that is not valid: nothing is fetched";
  return "the map could not be fetched: an old copy is never loaded in its place, and the next request tries again";
}

bool MapRequests::RequestGroup(const std::string& group, std::string* reason) {
  if (IsPathSafeGroupId(group)) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_group_ = group;
  }
  return Queue(group, std::string(), reason);
}

bool MapRequests::RequestMap(const std::string& id, std::string* reason) {
  const std::string group = SessionGroup();
  if (!IsValidMapId(id)) {
    *reason = "bad_id";
  } else if (group.empty()) {
    *reason = "no_session_group";
  } else {
    return Queue(group, id, reason);
  }
  Refused(group, id, *reason);
  return false;
}

bool MapRequests::Queue(const std::string& group, const std::string& id, std::string* reason) {
  if (!id.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    ids_[group].insert(id);
  }
  nevr_cdn::Env& env = env_;
  const bool queued = queue_.Request(
      group,
      [&env, group, id](const FetchReport& report) {
        const bool failed = report.outcome == FetchOutcome::kFailed;
        env.Event(failed ? nevr_cdn::EventLevel::kWarn : nevr_cdn::EventLevel::kInfo, "map_request",
                  {{"outcome", FetchOutcomeName(report.outcome)},
                   {"group", group},
                   {"id", id.empty() ? std::string("-") : id},
                   {"reason", report.reason.empty() ? std::string("-") : report.reason},
                   {"consequence", RequestConsequence(report, !id.empty())}});
      },
      reason);
  if (!queued) {
    Refused(group, id, *reason);
    return false;
  }
  WakeWorker();
  return true;
}

void MapRequests::Refused(const std::string& group, const std::string& id, const std::string& reason) {
  FetchReport refused;
  refused.reason = reason;
  env_.Event(nevr_cdn::EventLevel::kWarn, "map_request",
             {{"outcome", "refused"},
              {"group", IsPathSafeGroupId(group) ? group : std::string("-")},
              {"id", IsValidMapId(id) ? id : std::string("-")},
              {"reason", reason},
              {"consequence", RequestConsequence(refused, !id.empty())}});
}

bool MapRequests::RunNext(const Fetch& fetch) {
  return queue_.RunNext([&](const std::string& group) {
    FetchScope scope;
    scope.on_demand = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto asked = ids_.find(group);
      if (asked != ids_.end()) {
        scope.ids.assign(asked->second.begin(), asked->second.end());
        ids_.erase(asked);
      }
    }
    return fetch(group, scope);
  });
}

void MapRequests::SetWake(Wake wake) {
  std::lock_guard<std::mutex> lock(mutex_);
  wake_ = std::move(wake);
}

void MapRequests::WakeWorker() {
  Wake wake;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wake = wake_;
  }
  if (wake) wake();
}

void MapRequests::Stop() {
  stopped_.store(true);
  queue_.Stop();
}

std::string MapRequests::SessionGroup() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_group_;
}

namespace {

// The one installed entry, for the seams that take a plain function. Set and read through atomics: a session's request and the
// worker never see a half-installed entry.
std::atomic<MapRequests*> g_requests{nullptr};

void SessionGroupThunk(const std::string& group) {
  MapRequests* requests = g_requests.load(std::memory_order_acquire);
  std::string reason;
  if (requests != nullptr) requests->RequestGroup(group, &reason);
}

void FetchMapThunk(const std::string& id) {
  MapRequests* requests = g_requests.load(std::memory_order_acquire);
  std::string reason;
  if (requests != nullptr) requests->RequestMap(id, &reason);
}

}  // namespace

void InstallRequestSeams(MapRequests* requests) {
  g_requests.store(requests, std::memory_order_release);
  SetSessionGroupRequest(requests != nullptr ? &SessionGroupThunk : nullptr);
  nevr_map_session_pin::SetFetchRequest(requests != nullptr ? &FetchMapThunk : nullptr);
}

}  // namespace nevr_maps
