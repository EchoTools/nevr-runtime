// The Quest side's addresses for the peer removal log (integration/peer_removal_hook.h): CBroadcaster::DestroyPeer in libr15.so, the
// one function every removal of a peer goes through, reached from CleanupPeers' seven call sites through a PLT-style thunk.
//
// Per-binary table. Quest has one row here (libr15.so); the PC row is src/runtime/hook/addresses.h DESTROY_PEER (0x140F7BC60).
// Every value below is read from revault for libr15.so (project echovr, binary id 576) and from the lookup written in
// scratch/2026-11-12-drop-15s/CALLSITES.md, CALLSITES-2.md and CALLSITES-3.md. Sources, with each VA:
//
//   kDestroyPeerFnVa     0x2612CDC  the body: SBroadcasterData::DestroyPeer, reconstructed in src/reconstruction/r15/destroy_peer.cpp
//                                   (its declaration there is two-argument; CALLSITES-3 section 3 shows the calls pass three, and the raw
//                                   body reads two). Revault names the body DestroyPeer; that the thunk's target is this body is read from the
//                                   thunk's indirect jump, not from a name.
//   kDestroyPeerThunkVa  0x10533F0  the thunk CleanupPeers' seven calls go to: `bl 0x10533f0` (CALLSITES-2 section 2). Revault name:
//                                   thunk_FUN_02612cdc. Bytes (revault_disassemble): 70 3d 01 d0 | 11 72 40 f9 | 10 82 03 91 | 20 02 1f d6.
//   kDestroyPeerSlotVa   0x38010E0  the GOT word the thunk loads: `adrp x16,0x3801000; ldr x17,[x16,#0xe0]; add x16,x16,#0xe0; br x17`.
//                                   The page is 0x3801000 and the offset 0xe0 (kDestroyPeerSlotOffset below), so the slot is 0x38010E0.
//   kDestroyPeerSymbol           the dynamic symbol the slot's relocation names, read from libr15's string table (revault_search_strings
//                                   "DestroyPeer", libr15.so, VA 0xb69603): _ZN10NRadEngine16SBroadcasterData11DestroyPeerEyPKc. Its
//                                   arguments are (SBroadcasterData*, uint64, const char*), the calls' three. UNVERIFIED that the slot's
//                                   relocation is R_AARCH64_JUMP_SLOT against this name: the ELF relocation table was not read. The install
//                                   refuses (GotStatus) when the slot's relocation does not name it, so a wrong guess fails closed.
//
// The pinned check: the install stores the handler only when the slot holds kDestroyPeerFnVa at run time (GotTarget.expectedOriginal;
// got_hook.cpp refuses with kOriginalMismatch otherwise). The thunk's bytes are checked by peer_removal_hook_test.cpp, which decodes them to
// kDestroyPeerSlotVa and kDestroyPeerFnVa's page.
#pragma once

#include <cstdint>

namespace nevr_quest::integration::peer_removal {

inline constexpr const char* kDestroyPeerSymbol = "_ZN10NRadEngine16SBroadcasterData11DestroyPeerEyPKc";
inline constexpr std::uintptr_t kDestroyPeerFnVa = 0x2612CDCULL;
inline constexpr std::uintptr_t kDestroyPeerThunkVa = 0x10533F0ULL;
inline constexpr std::uintptr_t kDestroyPeerSlotVa = 0x38010E0ULL;
inline constexpr std::uintptr_t kDestroyPeerSlotPageVa = 0x3801000ULL;  // the adrp page of the thunk's first instruction
inline constexpr std::uintptr_t kDestroyPeerSlotOffset = 0xE0ULL;       // the ldr and add offsets of the thunk

// The reason argument is the third; the calls pass it in x2 (CALLSITES-2 section 2, the seven Quest sites).
using DestroyPeerSig = void(void* self, std::uint64_t index, const char* reason);

}  // namespace nevr_quest::integration::peer_removal
