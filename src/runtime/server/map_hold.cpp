#include "runtime/server/map_hold.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/logging.h"
#include "core/map_server_hold.h"
#include "runtime/compat/lobby_frame.h"
#include "runtime/server/join_hold.h"

namespace nevr_map_hold {
namespace {

// The legacy GameServerSessionStart frame's group GUID (messages.h): after the session GUID, at +0x10.
constexpr std::size_t kLegacyGroupOffset = 0x10;

std::atomic<RequestFetchFn> g_requestFetch{nullptr};
std::atomic<bool> g_signedOnlyWarned{false};

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

const char* Show(const std::string& text) { return text.empty() ? "none" : text.c_str(); }

}  // namespace

void SetRequestFetch(RequestFetchFn fn) { g_requestFetch.store(fn, std::memory_order_release); }

void OnSessionCreate(const std::string& group, const std::string& session) {
  const RequestFetchFn request = g_requestFetch.load(std::memory_order_acquire);
  if (request == nullptr) return;  // no channel (no maps.base_url): no hold, and nothing is logged
  nevr_join_hold::MapsEnable(true);  // a session create only reaches a dedicated server

  // The group is the one the server is allocated for (ADR 0048 decision 17): nothing else is fetched, whatever the config lists.
  const std::string want = nevr_maps::GroupToFetch(Lower(group));
  if (want.empty()) {
    nevr_join_hold::MapsArm(std::string());
    Log(EchoVR::LogLevel::Info,
        "[NEVR.MAPS] map hold off reason=no_group session=%s (the session starts on the stock maps)", Show(session));
    return;
  }
  if (nevr_join_hold::MapsArm(want) == nevr_join_hold::MapsArmResult::kMounted) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map hold off group=%s reason=mounted session=%s", want.c_str(), Show(session));
    return;
  }
  // Armed. The channel's worker fetches the group; a refusal (a bad id, a stopped queue) means nothing would mount it, so the
  // hold is dropped at once rather than held to its timeout.
  if (!request(want)) {
    nevr_join_hold::MapsArm(std::string());
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MAPS] map hold off group=%s reason=request_refused session=%s (the session starts without the group's maps)",
        want.c_str(), Show(session));
  }
}

void NoteFetchProgress(const nevr_maps::FetchProgress& progress) {
  nevr_maps::HoldPlan plan;
  plan.planned = true;  // a progress note comes after the plan (FetchGroup), so the sizes are known
  plan.set_bytes = progress.set_bytes;
  plan.bytes_to_fetch = progress.bytes_to_fetch;
  plan.bytes_fetched = progress.bytes_fetched;
  plan.elapsed_ms = progress.elapsed_ms;
  nevr_join_hold::MapsNoteProgress(progress.group, plan);
}

void NoteGroupMounted(const std::string& group, const nevr_maps::MountReport& report) {
  nevr_join_hold::MapsNoteMounted(group);
  std::size_t refused = 0;
  for (const auto& row : report.refused) {
    if (row.second == "signed_only") ++refused;
  }
  if (refused == 0 || g_signedOnlyWarned.exchange(true, std::memory_order_acq_rel)) return;
  Log(EchoVR::LogLevel::Warning, "%s", nevr_maps::SignedOnlyScriptsLine(group, refused).c_str());
}

std::string GroupOfSessionStart(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < kLegacyGroupOffset + nevr_lobby_frame::kGuidSize) return std::string();
  nevr_lobby_frame::Guid group{};
  std::copy(data + kLegacyGroupOffset, data + kLegacyGroupOffset + nevr_lobby_frame::kGuidSize, group.begin());
  if (nevr_lobby_frame::IsNil(group)) return std::string();
  return Lower(nevr_lobby_frame::FormatGuid(group));
}

}  // namespace nevr_map_hold
