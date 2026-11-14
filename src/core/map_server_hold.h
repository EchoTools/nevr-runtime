/* The dedicated server's maps hold (docs/adr/0048-custom-maps-channel.md decisions 13, 17 and 18): the pure decisions behind it.
 *
 * A server learns its lobby group from the session it is allocated for. When that group's maps are not mounted, the server
 * fetches them (the channel's worker) and holds joins until they mount, through the same queue as the levels hold
 * (runtime/server/join_hold_rules.h). The hold has its own timeout, computed from the set's size and the measured download rate
 * and capped by a named ceiling; network.join_hold_ms does not change it (it keeps meaning the levels-settle hold), and no
 * config key sets it. At the timeout the session starts on stock content and a Warning says so.
 *
 * Everything here is platform-free: no clock (the caller passes milliseconds), no lock, no Log(). The PC glue
 * (runtime/server/map_hold.cpp and join_hold.cpp) and the channel call these; the Quest host tests run them.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_maps {

// The timeout's bounds, in milliseconds. The values are the owner's choices for this first version and are not measured on a
// live server: the ceiling is two minutes, the bound the repository's own client login run gives a login before it counts as
// failed (AGENTS.md, "Client login test mechanics": --login-timeout defaults to 120 s); the floor is the least time a hold may
// wait, so a set the cache already holds still waits for its manifest's round trip; the slack is the manifest round trip and the
// mount, added to the transfer time. A measured live-run figure would replace any of these, not the rule that uses them.
inline constexpr std::uint64_t kMapHoldCeilingMs = 120000;
inline constexpr std::uint64_t kMapHoldFloorMs = 5000;
inline constexpr std::uint64_t kMapHoldSlackMs = 2000;

// What a fetch has shown of the group's set: the plan (known once the manifest is read) and the progress of the transfer. Its
// fields are the FetchProgress fields of core/map_run.h, without the group, so this file needs nothing from the fetch.
struct HoldPlan {
  bool planned = false;              // the manifest is read: the sizes below are known
  std::uint64_t set_bytes = 0;       // what the group's manifest lists for this platform
  std::uint64_t bytes_to_fetch = 0;  // the part of it the cache does not hold
  std::uint64_t bytes_fetched = 0;   // downloaded so far by this fetch
  std::uint64_t elapsed_ms = 0;      // since the fetch started
};

// The hold's timeout for a plan, in milliseconds, always within [kMapHoldFloorMs, kMapHoldCeilingMs]:
//   no plan yet, or no byte downloaded yet (no rate)  -> the ceiling (the size or the rate is unknown; wait as long as allowed)
//   nothing left to download                           -> the floor
//   otherwise                                          -> the time the remaining bytes take at the measured rate
//                                                         (bytes_fetched / elapsed_ms), plus the slack, clamped to the bounds
// The remaining bytes are bytes_to_fetch less bytes_fetched: the part the cache already holds is not waited for.
std::uint64_t MapHoldTimeoutMs(const HoldPlan& plan);

// The one group a server fetches: the group it is allocated for, and no other. The server's hosted-guild list (config `guilds`,
// `guilds: any`) is never an input, so no config key adds a group to the fetch. "" when the allocation names none.
std::string GroupToFetch(const std::string& allocated);

// The server's hold, for one session's group. Holding = joins wait. It is armed only for a group whose maps are not mounted.
class ServerMapHold {
 public:
  // Arms the hold for `group` at `nowMs`. False, and nothing held, for an empty group.
  bool Arm(const std::string& group, std::uint64_t nowMs);

  // Drops the hold (the session ended, or the server is shutting down).
  void Clear();

  // A progress note from the fetch of `group`. Ignored unless `group` is the held group.
  void Note(const std::string& group, const HoldPlan& plan);

  // The group's maps mounted: lifts the hold. kMounted once when it lifted the hold, kNone otherwise.
  enum class Step { kNone, kMounted, kTimeout };
  Step Mounted(const std::string& group);

  // At the deadline (armed time plus the current timeout) lifts the hold: kTimeout once, with the timeout and the plan that
  // expired it in TimeoutMs() and Plan() until the next Poll. kNone while the hold stands or when nothing is held.
  Step Poll(std::uint64_t nowMs);

  bool Holding() const { return holding_; }
  const std::string& Group() const { return group_; }
  std::uint64_t ArmedMs() const { return armedMs_; }
  const HoldPlan& Plan() const { return plan_; }
  std::uint64_t TimeoutMs() const { return timeoutMs_; }  // the timeout last computed by Poll (0 before the first)

 private:
  bool holding_ = false;
  std::string group_;
  std::uint64_t armedMs_ = 0;
  HoldPlan plan_;
  std::uint64_t timeoutMs_ = 0;
};

// How a held join is released. A join waits for the maps hold and for the levels hold (network.join_hold_ms, its own settle
// and timeout). It is released when neither holds it: the maps hold is lifted, and the levels hold is off, settled or timed out.
// `levelsEnabled` is network.join_hold_ms above 0; the other arguments are the levels gate's state at this moment.
enum class JoinHold { kHeld, kSettled, kTimeout };
inline JoinHold ComposeJoinHold(bool mapsHolding, bool levelsEnabled, bool levelsReady, bool levelsTimedOut) {
  if (mapsHolding) return JoinHold::kHeld;
  if (!levelsEnabled || levelsReady) return JoinHold::kSettled;
  if (levelsTimedOut) return JoinHold::kTimeout;
  return JoinHold::kHeld;
}

// The signed-only rule for a server's scripted maps (ADR 0048 decisions 7 and 18). `official` is true only when the manifest that
// pins the scripts was signed by a compiled-in official key; a key from config never makes it official. Script-free maps mount
// either way.
inline bool ServerMountsScripts(bool signedOnly, bool official) { return !signedOnly || official; }

// The Warning line at a hold's timeout (ADR 0048 decision 17): the group, the set's size, the bytes fetched, the timeout and the
// consequence. Never a hang: the session starts on stock content.
std::string MapHoldTimeoutLine(const std::string& group, const HoldPlan& plan, std::uint64_t timeoutMs);

// The Info line when a hold is armed: the group and the timeout it starts with.
std::string MapHoldArmedLine(const std::string& group, std::uint64_t timeoutMs);

// The Info line when a mount lifts the hold: the group and how long the joins waited.
std::string MapHoldMountedLine(const std::string& group, std::uint64_t waitedMs);

// The Warning for a signed-only server's scripted maps that the mount refused: "" when none was refused (decision 7: the
// script-free maps still mount, and the server says why the others did not).
std::string SignedOnlyScriptsLine(const std::string& group, std::size_t refused);

}  // namespace nevr_maps
