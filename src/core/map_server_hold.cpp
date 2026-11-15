#include "core/map_server_hold.h"

#include <algorithm>

#include "core/map_run.h"
#include <limits>

namespace nevr_maps {
namespace {

std::uint64_t Bounded(std::uint64_t ms) { return std::min(std::max(ms, kMapHoldFloorMs), kMapHoldCeilingMs); }

}  // namespace

std::uint64_t MapHoldTimeoutMs(const HoldPlan& plan) {
  if (!plan.planned) return kMapHoldCeilingMs;
  const std::uint64_t remaining = plan.bytes_to_fetch > plan.bytes_fetched ? plan.bytes_to_fetch - plan.bytes_fetched : 0;
  if (remaining == 0) return Bounded(kMapHoldSlackMs);
  if (plan.bytes_fetched == 0 || plan.elapsed_ms == 0) return kMapHoldCeilingMs;  // no rate measured yet
  // remaining bytes at bytes_fetched / elapsed_ms bytes per millisecond: remaining * elapsed_ms / bytes_fetched. The two checks
  // below only keep the arithmetic inside 64 bits (a value that would not fit is far past the ceiling); the ceiling itself is
  // Bounded's clamp.
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  if (remaining > kMax / plan.elapsed_ms) return kMapHoldCeilingMs;
  const std::uint64_t transfer = remaining * plan.elapsed_ms / plan.bytes_fetched;
  if (transfer > kMax - kMapHoldSlackMs) return kMapHoldCeilingMs;
  return Bounded(transfer + kMapHoldSlackMs);
}

std::string GroupToFetch(const std::string& allocated) { return allocated; }

bool ServerMapHold::Arm(const std::string& group, std::uint64_t nowMs) {
  Clear();
  if (group.empty()) return false;
  holding_ = true;
  group_ = group;
  armedMs_ = nowMs;
  return true;
}

void ServerMapHold::Clear() {
  holding_ = false;
  group_.clear();
  armedMs_ = 0;
  plan_ = HoldPlan{};
  timeoutMs_ = 0;
  updated_ = false;
}

void ServerMapHold::Note(const std::string& group, const HoldPlan& plan) {
  if (holding_ && group == group_) plan_ = plan;
}

ServerMapHold::Step ServerMapHold::Mounted(const std::string& group) {
  if (!holding_ || group != group_) return Step::kNone;
  holding_ = false;
  return Step::kMounted;
}

bool ServerMapHold::Updated(const std::string& group) {
  if (!holding_ || group != group_) return false;
  updated_ = true;
  return true;
}

ServerMapHold::Step ServerMapHold::Poll(std::uint64_t nowMs) {
  if (!holding_ || updated_) return Step::kNone;
  timeoutMs_ = MapHoldTimeoutMs(plan_);
  const std::uint64_t waited = nowMs >= armedMs_ ? nowMs - armedMs_ : 0;
  if (waited < timeoutMs_) return Step::kNone;
  holding_ = false;
  return Step::kTimeout;
}

std::string MapHoldTimeoutLine(const std::string& group, const HoldPlan& plan, std::uint64_t timeoutMs) {
  return "[NEVR.MAPS] map hold timeout group=" + group + " set_bytes=" + std::to_string(plan.set_bytes) +
         " bytes_fetched=" + std::to_string(plan.bytes_fetched) + " timeout_ms=" + std::to_string(timeoutMs) +
         " (the session starts on stock content; the group's maps mount when they arrive)";
}

std::string MapHoldArmedLine(const std::string& group, std::uint64_t timeoutMs) {
  return "[NEVR.MAPS] map hold armed group=" + group + " timeout_ms=" + std::to_string(timeoutMs) +
         " (joins wait until the group's maps mount, or the timeout passes)";
}

std::vector<std::string> UpdatedWhileMounted(const std::vector<std::pair<std::string, std::string>>& not_mounted) {
  std::vector<std::string> ids;
  for (const auto& row : not_mounted) {
    if (row.second == kMountedOtherVersionToken) ids.push_back(row.first);
  }
  return ids;
}

std::string MapUpdateRestartLine(const std::string& group, const std::vector<std::string>& ids) {
  std::string list;
  for (const std::string& id : ids) list += (list.empty() ? "" : ",") + id;
  return "[NEVR.MAPS] map update group=" + group + " ids=" + list +
         " reason=mounted_other_version (the channel lists a new version of a map this server has mounted, and a mounted map is "
         "never replaced in the process: no player joins the old version, and the server exits once its session is empty so its "
         "restart hosts the new one)";
}

std::string MapCheckFailedLine(const std::string& group) {
  return "[NEVR.MAPS] map check failed group=" + group +
         " (the group's manifest could not be fetched: the session goes ahead on the versions this server has mounted, and the "
         "next session checks again)";
}

std::string MapHoldMountedLine(const std::string& group, std::uint64_t waitedMs) {
  return "[NEVR.MAPS] map hold released group=" + group + " waited_ms=" + std::to_string(waitedMs) +
         " reason=mounted";
}

std::string SignedOnlyScriptsLine(const std::string& group, std::size_t refused) {
  if (refused == 0) return std::string();
  return "[NEVR.MAPS] signed_only group=" + group + " scripts_refused=" + std::to_string(refused) +
         " (-require-signed-plugins: a map's scripts mount only when an official key signed its manifest, and no official "
         "key is compiled in; the script-free maps mount)";
}

}  // namespace nevr_maps
