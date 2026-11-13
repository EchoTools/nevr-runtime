// The communication-timeout removal branch of CleanupPeers, per binary: the one call that removes a peer for the
// communication timeout, and the reason-string load that precedes it. A table of verified addresses and bytes, one
// row per binary; the rule that reads it is the same for both (peer_timeout_log_targets_test.cpp decodes each row).
//
// Sources (read-only lookup by the lookup seat, scratch/2026-11-12-drop-15s/CALLSITES.md; revault for both binaries):
//   PC echovr.exe, CBroadcaster::CleanupPeers at 0x140F76500 (revault, 418 instructions):
//     call 0x140F76889: CALL 0x140F7BC60 (CBroadcaster::DestroyPeer), bytes e8 d2 53 00 00, after CMP RBP,[RDI+0x488]
//     at 0x140F76873 and JC 0x140F76893 (threshold 15,000,000 us).
//     reason load 0x140F7687C: LEA R8,[RIP+0xd59afd] -> 0x141CD0380 "communication timeout".
//   Quest libr15.so, SBroadcasterData::CleanupPeers at 0x25EF9B0 (revault, 315 instructions):
//     call 0x25EFCA4: bl 0x010533F0, bytes d3 8d a9 97, after ldr x9,[x19,#0x488]; cmp; b.cs 0x25efc94 at 0x25EFB2C..0x25EFB34.
//     reason load 0x25EFCA0: add x2,x2,#0xf81 (adrp x2 at 0x25EFC94 to page 0x32E8000) -> 0x32E8F81 "communication timeout".
//     The callee 0x10533F0 is named thunk_FUN_02612cdc by revault. Its declaration in src/reconstruction/r15/cleanup_peers.cpp
//     is SBroadcasterData::DestroyPeer(void*, uint64_t); that the thunk IS DestroyPeer is UNVERIFIED beyond the declaration
//     and the reason-string match.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nevr_peer_timeout_log {

struct CallSiteRow {
    const char* binary;                 // the module the addresses belong to
    std::uintptr_t call_va;             // the removal call instruction (virtual address in the binary's image)
    std::array<std::uint8_t, 5> call_bytes;  // the bytes at call_va in an unpatched binary
    std::size_t call_len;               // how many of call_bytes are the instruction (PC: CALL rel32, 5; Quest: BL, 4)
    std::uintptr_t callee_va;           // the call's target: DestroyPeer (PC verified; Quest unverified beyond the declaration)
    std::uintptr_t reason_load_va;      // the instruction that loads the reason string's address
    std::uintptr_t reason_va;           // the reason string: "communication timeout"
};

// PC: CALL rel32, five bytes. The call's rel32 is 0x000053D2, so the target is 0x140F76889 + 5 + 0x53D2 = 0x140F7BC60.
inline constexpr CallSiteRow kPcCallSite{
    "echovr.exe",
    0x140F76889ULL,
    {0xE8, 0xD2, 0x53, 0x00, 0x00},
    5,
    0x140F7BC60ULL,
    0x140F7687CULL,
    0x141CD0380ULL,
};

// Quest: BL imm26, four bytes. The word 0x97A98DD3 is the BL; its imm26 is -0x56722D (x4 = -0x159C8B4), so
// 0x25EFCA4 + imm*4 = 0x10533F0.
inline constexpr CallSiteRow kQuestCallSite{
    "libr15.so",
    0x25EFCA4ULL,
    {0xD3, 0x8D, 0xA9, 0x97, 0x00},
    4,
    0x10533F0ULL,
    0x25EFCA0ULL,
    0x32E8F81ULL,
};

}  // namespace nevr_peer_timeout_log
