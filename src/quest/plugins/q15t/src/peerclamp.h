// peerclamp (source build RE-34 G29, the source build's peerclamp; row R36; TEST ONLY, default OFF, option Q15T_PEERCLAMP): a peer's
// last-received stamp that is above the broadcaster's "now" is pulled down to "now" before the stock timeout loop runs.
//
// The crash it guards: SBroadcasterData::CleanupPeers computes age = bd[+0x478] ("now", us) - entry[+0x228] (the peer's
// last-received stamp) as an unsigned 64-bit subtract with no clamp (disassembly 0x24efa74 ldr x24,[x19,#0x478]; 0x24efa94 ldr
// x25,[x8,#0x228]; 0x24efae4 sub x25,x24,x25). A stamp above "now" reads as ~2^64 us of silence, every timeout branch fires at
// once and the new game-server peer is destroyed as "communication timeout" 0.25 s after connect (W-1 gun11 launch 7).
// Wraps libr15's JUMP_SLOT for CleanupPeers (ELF 0x36efad8 -> the function at ELF 0x24ef9b0; its one caller is
// CBroadcaster::Receive(uint), x0 = the SBroadcasterData). The wrapper walks the peer list the way the stock loop does and,
// for every entry whose key is not CSysNet::SAddress::kInvalid (the entries the stock loop tests), lowers a stamp above "now"
// to "now"; then it calls the stock function. A stamp <= "now" (every normal peer, a real silence included) is never touched,
// and "now" is never written. Same thread as the stock loop; no lock, no allocation, no engine call. Every read is a SafeRead;
// the only game-memory write is the 8-byte stamp, at an address that was just read. Data-page hook only.
// Details, evidence and prediction: docs/quest/q15t/modules/peerclamp.md.
#pragma once

#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

inline constexpr std::uintptr_t kPeerClampSlot = 0x36efad8;
inline constexpr std::uintptr_t kPeerClampReal = 0x24ef9b0;
inline constexpr std::uintptr_t kPeerClampInvalid = 0x371d538;  // GOT word holding &CSysNet::SAddress::kInvalid (disassembly 0x24efa14/18)

// Refused (one ERROR line, nothing swapped) when the kInvalid pointer cannot be read or is null, or the slot does not hold the
// pinned function.
inline constexpr WordPin kPeerClampWordPins[] = {
    {0x24ef9b0, 0x6db733ed},
    {0x24ef9b4, 0x6d012beb},
    {0x24ef9d8, 0xb945541b},
    {0x24efa14, 0xd0009177},
    {0x24efa18, 0xf9429ef7},
    {0x24efa2c, 0xaa0003f3},
    {0x24efa30, 0x5281291c},
    {0x24efa68, 0xb9456268},
    {0x24efa6c, 0xf9428e69},
    {0x24efa74, 0xf9423e78},
    {0x24efa78, 0x1ac80b6a},
    {0x24efa7c, 0xf86a5934},
    {0x24efa80, 0x1b08ed5a},
    {0x24efa84, 0x52801002},
    {0x24efa8c, 0x9bbc5348},
    {0x24efa90, 0xb940091b},
    {0x24efa94, 0xf9411519},
    {0x24efa98, 0x91006116},
    {0x24efaa4, 0xb4fffd40},
    {0x24efae4, 0xcb190319},
    {0x24eeec8, 0xf9400260},
    {0x24eeecc, 0x97a90645},
    {0xf307e0, 0xf0013df0},
    {0xf307e4, 0xf9456e11},
    {0xf307e8, 0x912b6210},
    {0xf307ec, 0xd61f0220},
};
inline constexpr std::size_t kPeerClampWordPinCount = sizeof kPeerClampWordPins / sizeof kPeerClampWordPins[0];
// True when every pin matches at base + its vaddr (one ERROR line naming the first that does not). No MD5 or other file-hash gate.
bool PeerClampWordPinsMatch(std::uintptr_t base);

InstallResult InstallPeerClamp(std::uintptr_t base);
InstallResult InstallPeerClampAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr, std::uintptr_t invalid_vaddr);

// Stamps clamped so far.
unsigned PeerClampCount();

void ResetPeerClampForTest();

}  // namespace q15t
