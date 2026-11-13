// The communication-timeout call site per binary (runtime/patch/peer_timeout_log_targets.h).
//
// What these tests show: each row's addresses and bytes decode to each other, for both binaries: the call at call_va
// reaches callee_va, and the reason load at reason_load_va yields reason_va. The bytes are the ones the lookup read
// from the binaries (CALLSITES.md); the game binaries are not in this repo, so the decode is checked against those
// fixtures, not against a live image. The Quest callee is named by the decompile declaration only (UNVERIFIED in the
// header); the test checks the call reaches the address the table names, not that the address is DestroyPeer.
//
// Mutants (one line each, against peer_timeout_log_targets.h):
//   M1 PC callee_va off by 4                       -> PcCallReachesDestroyPeer fails.
//   M2 Quest call_len 4 -> 5                       -> QuestCallSiteLengthIsTheBl fails.
//   M3 Quest reason_va off by one                  -> QuestReasonLoadYieldsTheString fails.

#include <gtest/gtest.h>

#include <cstdint>

#include "runtime/patch/peer_timeout_log_targets.h"

namespace {

using nevr_peer_timeout_log::kPcCallSite;
using nevr_peer_timeout_log::kQuestCallSite;

// The PC reason load at 0x140F7687C: LEA R8,[RIP+0xd59afd] (bytes 4c 8d 05 fd 9a d5 00), 7 bytes; the next
// instruction is at 0x140F76883.
constexpr std::uint8_t kPcReasonLea[] = {0x4C, 0x8D, 0x05, 0xFD, 0x9A, 0xD5, 0x00};
constexpr std::uintptr_t kPcReasonLeaVa = 0x140F7687CULL;
constexpr std::uintptr_t kPcReasonLeaLen = sizeof(kPcReasonLea);

// The Quest call at 0x25EFCA4 is BL; the bytes d3 8d a9 97 read little-endian give the word 0x97A98DD3.
constexpr std::uint32_t kQuestBlWord = 0x97A98DD3U;
constexpr std::uint32_t kQuestAddWord = 0x913E0442U;   // add x2, x2, #0xf81 (the reason load at 0x25EFCA0)
constexpr std::uint32_t kQuestAdrpWord = 0xB00067C2U;  // adrp x2, page (at 0x25EFC94)
constexpr std::uintptr_t kQuestAdrpVa = 0x25EFC94ULL;

std::uint32_t WordOf(const std::uint8_t* b) {
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

TEST(PeerTimeoutLogTargets, PcCallReachesDestroyPeer) {
    ASSERT_EQ(kPcCallSite.call_len, 5u);
    ASSERT_EQ(kPcCallSite.call_bytes[0], 0xE8);
    const std::uint32_t rel = WordOf(&kPcCallSite.call_bytes[1]);
    const std::uintptr_t target = kPcCallSite.call_va + kPcCallSite.call_len + rel;
    EXPECT_EQ(target, kPcCallSite.callee_va);
    EXPECT_EQ(kPcCallSite.callee_va, 0x140F7BC60ULL);
}

TEST(PeerTimeoutLogTargets, PcReasonLoadYieldsTheString) {
    ASSERT_EQ(kPcCallSite.reason_load_va, kPcReasonLeaVa);
    const std::uint32_t disp = WordOf(&kPcReasonLea[3]) & 0xFFFFFFFFU;  // disp32 of LEA R8,[RIP+disp]
    const std::uintptr_t target = kPcReasonLeaVa + kPcReasonLeaLen + disp;
    EXPECT_EQ(target, kPcCallSite.reason_va);
    EXPECT_EQ(kPcCallSite.reason_va, 0x141CD0380ULL);
}

TEST(PeerTimeoutLogTargets, QuestCallSiteLengthIsTheBl) {
    EXPECT_EQ(kQuestCallSite.call_len, 4u);
    EXPECT_EQ(WordOf(kQuestCallSite.call_bytes.data()), kQuestBlWord);
    EXPECT_EQ(kQuestBlWord & 0xFC000000U, 0x94000000U);  // BL opcode
}

TEST(PeerTimeoutLogTargets, QuestCallReachesTheTableCallee) {
    std::int32_t imm26 = static_cast<std::int32_t>(kQuestBlWord & 0x03FFFFFFU);
    if (imm26 & 0x02000000) imm26 -= 0x04000000;  // sign-extend the 26-bit word offset
    const std::uintptr_t target = kQuestCallSite.call_va + static_cast<std::intptr_t>(imm26) * 4;
    EXPECT_EQ(target, kQuestCallSite.callee_va);
    EXPECT_EQ(kQuestCallSite.callee_va, 0x10533F0ULL);
}

TEST(PeerTimeoutLogTargets, QuestReasonLoadYieldsTheString) {
    ASSERT_EQ(kQuestCallSite.reason_load_va, 0x25EFCA0ULL);
    // adrp x2, page: immlo = bits 29..30, immhi = bits 5..23; the page is (adrp VA & ~0xFFF) + imm << 12.
    const std::uint32_t immlo = (kQuestAdrpWord >> 29) & 0x3U;
    const std::uint32_t immhi = (kQuestAdrpWord >> 5) & 0x7FFFFU;
    const std::uint32_t imm21 = (immhi << 2) | immlo;
    const std::uintptr_t page = (kQuestAdrpVa & ~0xFFFULL) + (static_cast<std::uintptr_t>(imm21) << 12);
    EXPECT_EQ(page, 0x32E8000ULL);
    // add x2, x2, #imm12: imm12 is bits 10..21.
    const std::uint32_t imm12 = (kQuestAddWord >> 10) & 0xFFFU;
    EXPECT_EQ(page + imm12, kQuestCallSite.reason_va);
    EXPECT_EQ(kQuestCallSite.reason_va, 0x32E8F81ULL);
}

TEST(PeerTimeoutLogTargets, RowsNameTheirBinaries) {
    EXPECT_STREQ(kPcCallSite.binary, "echovr.exe");
    EXPECT_STREQ(kQuestCallSite.binary, "libr15.so");
}

}  // namespace
