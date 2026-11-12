// The Quest peer removal log: libr15's CBroadcaster::DestroyPeer (peer_removal_targets.h) is hooked through its GOT slot, the way
// combat/peerclamp.cpp hooks CleanupPeers. Each call logs the removed peer's slot and reason, at the reason's level
// (src/runtime/compat/peer_timeout_log.h: LevelForRemovalReason, RecognizedRemovalReason; the same functions the PC hook uses), then
// calls the game's function with the same arguments.
//
// Not in this log: the peer's address text and the gap since the game last heard from the peer. Their offsets are not pinned on
// Quest (the CleanupPeers reads of +0x228 and +0x478 are established; the address field's reads in the reconstruction are not), so the
// record omits them rather than guess.
//
// Install (post-load, non-combat): InstallPeerRemovalHook is the post-load action's body (production_steps.cpp). A refusal is a
// GotStatus and a sentinel line; the game's own calls stay as they were.
#pragma once

#include <cstdint>

#include "quest/sentinel/got_hook.h"

namespace nevr_quest::integration::peer_removal {

// Installs the hook into libr15's DestroyPeer slot with the production image lookup. kModuleNotLoaded until libr15 is loaded.
sentinel::GotStatus InstallPeerRemovalHook() noexcept;

// The same with an image lookup a test passes.
sentinel::GotStatus InstallPeerRemovalHookAt(sentinel::ImageLookup lookup) noexcept;

// True when the hook is in the slot.
bool Installed() noexcept;

// Logs one removal at its reason's level (the handler calls this before the game's function).
void LogRemoval(std::uint64_t index, const char* reason) noexcept;

// Test seam: forgets the installed hook and its state.
void ResetForTest() noexcept;

}  // namespace nevr_quest::integration::peer_removal
