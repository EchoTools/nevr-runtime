// Peer timeout logging, PC hooks (the rules are in runtime/patch/peer_timeout_log_rules.h).
//
// Server: CBroadcaster::DestroyPeer (addresses.h DESTROY_PEER) is hooked. CleanupPeers is its only caller, and the game
// passes "communication timeout" as the reason only on CleanupPeers' removal branch; the hook writes one Warning for
// that reason and forwards every argument to the original.
//
// Client: CBroadcaster::Send (addresses.h BROADCASTER_SEND) is hooked to record the time of each send to the host, and
// the NetGame state hook reports entering and leaving the game. The frame tick calls CheckNoSend once per frame.
//
// The Quest half is src/quest/integration/peer_timeout_log.h: it hooks libr15's DestroyPeer through its GOT slot and logs
// each removal at the level the same rules give.

#pragma once

#include <array>
#include <cstdint>

#include "abi/echovr.h"
#include "abi/echovr_functions.h"

namespace nevr_peer_timeout_log {

// The first 18 bytes of CBroadcaster::DestroyPeer (0x140F7BC60): MOV [RSP+0x18],RBX; MOV [RSP+0x20],RSI; PUSH RDI;
// SUB RSP,0x50; MOV R11,R8. All relocatable.
inline constexpr std::array<std::uint8_t, 18> kDestroyPeerPrologue{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x4D, 0x8B, 0xD8};

// The first 16 bytes of CBroadcaster::Send (0x140F89AF0): MOV RAX,RSP; MOV [RAX+8],RBX; MOV [RAX+0x20],R9;
// MOV [RAX+0x18],R8D; PUSH RBP. All relocatable.
inline constexpr std::array<std::uint8_t, 16> kBroadcasterSendPrologue{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x4C, 0x89, 0x48, 0x20, 0x44, 0x89, 0x40, 0x18, 0x55};

// The game's functions as the hooks found them, set by Install before PatchDetour.
using DestroyPeerFn = void (*)(std::int64_t* self, std::uint64_t index, const char* reason);
extern DestroyPeerFn OriginalDestroyPeer;
extern EchoVR::BroadcasterSendFunc* OriginalBroadcasterSend;

// Installs the hooks for this run's role: DestroyPeer on both roles, Send on a client only. Each prologue is checked before
// anything is written; a mismatch is logged as a Warning and that hook is not installed. Returns true when the hook is
// in place.
bool Install(std::uintptr_t gameBase, bool isServer);

// Detours (exposed for the wiring test; not called from anywhere else).
void DestroyPeerHook(std::int64_t* self, std::uint64_t index, const char* reason);
INT32 BroadcasterSendHook(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId messageId, INT32 mbThreadPriority,
                          VOID* item, UINT64 size, VOID* buffer, UINT64 bufferLen, EchoVR::Peer peer, UINT64 dest,
                          FLOAT priority, EchoVR::SymbolId unk);

// The client's NetGame state, reported by the NetGame switch-state hook.
void OnNetGameState(bool inGame);

// Called once per frame on the game thread: a client silent for the threshold logs one Warning per silence.
void CheckNoSend();

}  // namespace nevr_peer_timeout_log
