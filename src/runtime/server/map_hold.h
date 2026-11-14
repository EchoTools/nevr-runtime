#pragma once

// The dedicated server's maps hold, PC glue (docs/adr/0048-custom-maps-channel.md decisions 13, 17 and 18). The rules are
// core/map_server_hold.h; the hold's state and its join queue are join_hold.cpp's (the maps hold is one more reason a join waits
// for, beside the levels hold). This file is the seam between the session and the channel:
//
//   game thread   the session create (gameserver_callbacks.cpp, kLobbySessionCreate's group id, and the legacy session start's
//                 group GUID) calls OnSessionCreate: the one group the server is allocated for is fetched when its maps are not
//                 mounted, and joins wait for them, or for the timeout.
//   channel       its worker calls NoteFetchProgress from FetchGroup's progress callback, and NoteGroupMounted after its mount has
//                 run for the group. The channel sets RequestFetch at its start.
//
// The channel is the one side that fetches: this file asks it through RequestFetch and never touches the cache. Without a
// RequestFetch (no maps.base_url, so no channel) nothing here holds a join and nothing is logged.

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/map_run.h"

namespace nevr_map_hold {

/// The channel's request entry (ADR 0048 decision 17). It queues one group's fetch on the channel's worker (the GroupFetchQueue
/// of core/map_fetch_queue.h, whose Request refuses a path-unsafe group id and a stopped queue). True when the request is queued;
/// false when it is refused, and then the server holds nothing for the session. Any thread; null means no channel.
using RequestFetchFn = bool (*)(const std::string& group);

/// Sets the channel's request entry. Called once at the channel's start. Setting it turns the maps hold on for a dedicated server.
void SetRequestFetch(RequestFetchFn fn);

/// A session was created on this server, with the group the service named for it ("" when none). Game thread, once per session
/// create. Arms the maps hold for the group when its maps are not mounted and asks the channel to fetch them; logs why not
/// otherwise (`session` only goes into the Info line).
void OnSessionCreate(const std::string& group, const std::string& session);

/// A fetch's progress for one group, from the channel's worker: the plan once the manifest is read, then after each piece.
void NoteFetchProgress(const nevr_maps::FetchProgress& progress);

/// The channel's mount has run and the group's maps are mounted in this process (the report is the mount's: its signed_only
/// refusals are counted, and one Warning per process says that the scripted maps were refused). Any thread.
void NoteGroupMounted(const std::string& group, const nevr_maps::MountReport& report);

/// The group a legacy GameServerSessionStart frame names: the GUID at its +0x10 (messages.h, the frame's two GUIDs), in the same
/// lowercase text the protobuf group id is taken as. "" when the frame is too short to carry it.
std::string GroupOfSessionStart(const std::uint8_t* data, std::size_t size);

}  // namespace nevr_map_hold
