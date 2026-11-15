#pragma once

// Join hold on the PC dedicated server (#1737; rules in join_hold_rules.h, the game's functions in
// docs/reference/join-hold.md). With network.join_hold_ms above 0, the server's forward of the service's accept
// (LobbyAcceptPlayersSuccessV2) waits until the session's levels are seen enabled, then goes to the game in arrival
// order. Install runs once from RunDeferredRuntimeBootstrap (boot.cpp) after the config is loaded; a client or a
// server with the key off logs that it does not apply (N122/N124) and no hook is installed. Admit, ReleaseDue and the
// session calls run on the game thread (GameServerLib::Update's ProcessReceivedMessages); the two detours run on the
// game's own threads, so the state is behind one mutex held for constant-time updates only.

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/map_server_hold.h"

namespace nevr_join_hold {

void Install(bool isServer);

/// True while a join-hold detour is installed on CncaGameSpace::LoadLevel (the load timing hook then stays out of its way).
/// The flag is set when the LoadLevel site attaches and cleared only when Retire removed that detour, so a detour that stays
/// installed is still reported and the timing hook does not add a second one. It reads site 1 of kSites in join_hold.cpp,
/// so reordering the sites means updating kLoadLevelSite there.
bool HooksLoadLevel();

/// True when the accept was held (a copy was queued): the caller must not forward it now. False: forward it now.
/// `entrants` is how many entrants it names; `sessionId` only goes into the log.
bool Admit(const void* encoded, std::size_t size, std::size_t entrants, const char* sessionId);

/// Forwards what is due to `broadcaster` (an EchoVR::Broadcaster*): all held accepts, in order, once the levels are
/// enabled, or at the timeout. One atomic read when nothing is held.
void ReleaseDue(void* broadcaster, const char* sessionId);

/// A session was created on this server (arms the hold; an earlier session's held accepts are dropped).
void OnSessionCreate(const char* sessionId);

/// The session ended (`cause` is service_ended or game_ended) or the server is shutting down: disarm and drop.
void OnSessionEnd(const char* cause, const char* sessionId);

// The maps hold (ADR 0048 decision 17; the rules are core/map_server_hold.h, the callers are runtime/server/map_hold.cpp). It is
// one more reason a join waits for, beside the levels hold above, and it does not depend on network.join_hold_ms: a join waits
// for both (nevr_maps::ComposeJoinHold). The state sits behind the same mutex as the levels gate.

enum class MapsArmResult { kArmed, kMounted, kNoGroup };

/// The channel is set (a server with a fetch channel): the gate is in use, with no game hook installed for the maps hold. A
/// client does nothing here.
void MapsEnable(bool isServer);

/// A session's group: arms the maps hold for it when the group's maps are not mounted in this process (kArmed, and an Info line
/// with the timeout it starts with); kMounted when they are (the hold is dropped); kNoGroup for "" (the hold is dropped). Any
/// earlier maps hold is replaced. Game thread.
MapsArmResult MapsArm(const std::string& group);

/// A fetch's plan or progress for `group`, from the channel's worker. Ignored unless `group` is the held group.
void MapsNoteProgress(const std::string& group, const nevr_maps::HoldPlan& plan);

/// The group's maps are mounted in this process: remembered (a later session of the group does not hold for it), and a held
/// group's hold is lifted with an Info line. Any thread.
void MapsNoteMounted(const std::string& group);

}  // namespace nevr_join_hold
