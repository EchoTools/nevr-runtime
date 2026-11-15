#include "core/map_request.h"

#include <atomic>
#include <cstdio>
#include <utility>
#include <vector>

#include "core/map_format.h"
#include "core/map_groups.h"
#include "core/map_manifest.h"
#include "core/map_session_pin.h"

namespace nevr_maps {
namespace {

std::string SizeText(std::uint64_t bytes) {
  char text[32];
  if (bytes >= (1ull << 30)) {
    std::snprintf(text, sizeof text, "%.1f GiB", static_cast<double>(bytes) / static_cast<double>(1ull << 30));
  } else {
    std::snprintf(text, sizeof text, "%llu MiB", static_cast<unsigned long long>((bytes + (1ull << 20) - 1) >> 20));
  }
  return text;
}

}  // namespace

std::string RequestConsequence(const FetchReport& report, bool named_map, bool is_server, bool mounts_now) {
  const std::string& reason = report.reason;
  if (report.outcome != FetchOutcome::kFailed) {
    if (is_server) return "the group's maps are stored and mount now";
    if (report.maps_updated > 0) {
      return mounts_now ? "the map is stored and mounts now, no restart: join again to play"
                        : "the map is stored: restart the game to play it (a map downloaded mid-run mounts at the next start)";
    }
    if (!named_map) return "the maps you hold of this group are the current versions";
    return "your copy is the current version: a server running another version cannot be joined until it updates";
  }
  if (reason == "not_listed") {
    return "this map is not in your lobby group's map list: a player pulls only the maps its group offers, and an old version is "
           "never fetched, so it cannot be played";
  }
  if (reason == "cancelled" || reason == "stopped") return "the game is closing: nothing more is fetched";
  if (reason == "no_session_group") return "no lobby group is known for this session, so the map cannot be asked for";
  if (reason == "bad_group" || reason == "bad_id") return "the session named an id that is not valid: nothing is fetched";
  if (reason == "over_budget") {
    return "not enough room for " + report.no_room_id + ": needs " + SizeText(report.no_room_needed) + ", " +
           SizeText(report.no_room_available) + " can be made free, cap " +
           (report.no_room_cap == 0 ? std::string("none") : SizeText(report.no_room_cap)) +
           ": no maps were removed for it, and an old copy is never loaded in its place";
  }
  return "the map could not be fetched: an old copy is never loaded in its place, and the next request tries again";
}

bool MapRequests::RequestGroup(const std::string& group, std::string* reason) {
  if (!is_server_ && IsPathSafeGroupId(group)) {
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
  const bool is_server = is_server_;
  const bool mounts_now = mounts_now_;
  nevr_cdn::Env& env = env_;
  const bool queued = queue_.Request(
      group,
      [&env, is_server, mounts_now, group, id](const FetchReport& report) {
        const bool failed = report.outcome == FetchOutcome::kFailed;
        env.Event(failed ? nevr_cdn::EventLevel::kWarn : nevr_cdn::EventLevel::kInfo, "map_request",
                  {{"outcome", FetchOutcomeName(report.outcome)},
                   {"group", group},
                   {"id", id.empty() ? std::string("-") : id},
                   {"reason", report.reason.empty() ? std::string("-") : report.reason},
                   {"consequence", RequestConsequence(report, !id.empty(), is_server, mounts_now)}});
      },
      reason);
  if (!queued) Refused(group, id, *reason);
  return queued;
}

void MapRequests::Refused(const std::string& group, const std::string& id, const std::string& reason) {
  FetchReport refused;
  refused.reason = reason;
  env_.Event(nevr_cdn::EventLevel::kWarn, "map_request",
             {{"outcome", "refused"},
              {"group", IsPathSafeGroupId(group) ? group : std::string("-")},
              {"id", IsValidMapId(id) ? id : std::string("-")},
              {"reason", reason},
              {"consequence", RequestConsequence(refused, !id.empty(), is_server_)}});
}

bool MapRequests::RunNext(const Fetch& fetch) {
  return queue_.RunNext([&](const std::string& group) {
    FetchScope scope;
    scope.on_demand = !is_server_;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto asked = ids_.find(group);
      if (asked != ids_.end()) {
        scope.ids.assign(asked->second.begin(), asked->second.end());
        ids_.erase(asked);
      }
    }
    FetchReport report = fetch(group, scope);
    // A join held for a named map leaves at once on a failure instead of waiting out its deadline.
    if (report.outcome == FetchOutcome::kFailed) {
      const std::string consequence = RequestConsequence(report, true, is_server_);
      for (const std::string& id : scope.ids) nevr_map_session_pin::NoteFetchFailed(id, consequence);
    }
    return report;
  });
}

std::string MapRequests::SessionGroup() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_group_;
}

namespace {

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

bool RequestGroupThroughSeam(const std::string& group) {
  MapRequests* requests = g_requests.load(std::memory_order_acquire);
  std::string reason;
  return requests != nullptr && requests->RequestGroup(group, &reason);
}

}  // namespace nevr_maps
