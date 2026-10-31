// P13 (#1774): the allow_incoming detour on CBroadcaster::Initialize.
//
// The pure rule (ForceAllowIncoming) is tested on its own. The detour itself is tested under MinHook on a stand-in
// with the game function's 15-parameter signature: the production BroadcasterInitializeHook is attached to the
// stand-in, and a call through the stand-in's address must reach the stand-in with bit 0 set, every stack argument
// unchanged, and the return value passed back. The install's prologue check and the server escalation are in
// mode_patches.cpp and are covered by tools/tests/test_broadcaster_allow_incoming_wiring.py.
//
// Mutants (one line each, run against this file):
//   M1 ForceAllowIncoming: drop `*mask |= kAllowIncomingBit`        -> PureRule and the detour test fail on bit 0.
//   M2 detour: forward `p15` as `0` in the call to the original     -> the detour test fails on the last stack argument.
//   M3 detour: `return OriginalBroadcasterInitialize(...)` -> `return 0` -> the detour test fails on the return value.
//   M4 detour: drop the `ForceAllowIncoming(mask)` call              -> the detour test fails on bit 0.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>

#include "core/hooking.h"
#include "runtime/patch/broadcaster_allow_incoming.h"
#include "runtime/server/broadcaster_config.h"

namespace {

using nevr_broadcaster_allow_incoming::BroadcasterInitializeFn;
using nevr_broadcaster_allow_incoming::BroadcasterInitializeHook;
using nevr_broadcaster_allow_incoming::OriginalBroadcasterInitialize;

TEST(BroadcasterConfig, ForceAllowIncomingSetsBitZeroAndKeepsTheRest) {
  std::uint32_t mask = 0x0FFEu;  // bits 1-11 set, bit 0 clear
  EXPECT_TRUE(nevr_broadcaster_config::ForceAllowIncoming(&mask));
  EXPECT_EQ(mask, 0x0FFFu);

  std::uint32_t all = 0xFFFFFFFEu;
  EXPECT_TRUE(nevr_broadcaster_config::ForceAllowIncoming(&all));
  EXPECT_EQ(all, 0xFFFFFFFFu);
}

TEST(BroadcasterConfig, ForceAllowIncomingLeavesAMaskWithTheBitSetAlone) {
  std::uint32_t mask = 0x0807u;
  EXPECT_FALSE(nevr_broadcaster_config::ForceAllowIncoming(&mask));
  EXPECT_EQ(mask, 0x0807u);
}

TEST(BroadcasterConfig, ForceAllowIncomingLeavesANullMaskAlone) {
  EXPECT_FALSE(nevr_broadcaster_config::ForceAllowIncoming(nullptr));
}

// What the stand-in saw on its last call.
struct StandInCall {
  std::int64_t* self = nullptr;
  std::uint64_t nameHash = 0;
  std::int16_t port = 0;
  std::uint32_t maskSeen = 0;
  std::uint64_t stack[11] = {};
};
StandInCall g_call;

constexpr std::uint64_t kStandInReturn = 0x0123456789ABCDEFull;

// Stands in for CBroadcaster::Initialize: same 15 parameters, all recorded, a fixed return value.
[[gnu::noinline, gnu::noipa]] std::uint64_t StandInInitialize(std::int64_t* self, std::uint64_t nameHash, std::int16_t port,
                                                  std::uint32_t* mask, std::uint64_t p5, std::uint64_t p6,
                                                  std::uint64_t p7, std::uint64_t p8, std::uint64_t p9,
                                                  std::uint64_t p10, std::uint64_t p11, std::uint64_t p12,
                                                  std::uint64_t p13, std::uint64_t p14, std::uint64_t p15) {
  g_call.self = self;
  g_call.nameHash = nameHash;
  g_call.port = port;
  g_call.maskSeen = mask != nullptr ? *mask : 0u;
  g_call.stack[0] = p5;
  g_call.stack[1] = p6;
  g_call.stack[2] = p7;
  g_call.stack[3] = p8;
  g_call.stack[4] = p9;
  g_call.stack[5] = p10;
  g_call.stack[6] = p11;
  g_call.stack[7] = p12;
  g_call.stack[8] = p13;
  g_call.stack[9] = p14;
  g_call.stack[10] = p15;
  return kStandInReturn;
}

TEST(BroadcasterAllowIncomingHook, TheDetourForcesBitZeroAndForwardsEveryParameter) {
  ASSERT_TRUE(Hooking::Initialize());

  // Hook the stand-in exactly as PatchEnableServer hooks the game function: the trampoline is published into the
  // pointer the detour calls through.
  OriginalBroadcasterInitialize = reinterpret_cast<BroadcasterInitializeFn>(reinterpret_cast<void*>(&StandInInitialize));
  ASSERT_EQ(Hooking::Attach(reinterpret_cast<PVOID*>(&OriginalBroadcasterInitialize),
                            reinterpret_cast<PVOID>(BroadcasterInitializeHook)),
            TRUE)
      << "MinHook refused the stand-in: " << Hooking::LastAttachError();

  // Call through the stand-in's own address, which the hook now redirects to the detour.
  BroadcasterInitializeFn volatile target =
      reinterpret_cast<BroadcasterInitializeFn>(reinterpret_cast<void*>(&StandInInitialize));
  std::int64_t self = 0;
  std::uint32_t mask = 0x0806u;  // bits 1, 2 and 11 set, bit 0 (allow_incoming) clear, as the netconfig gives it
  const std::uint64_t ret = target(&self, 0xABCDu, static_cast<std::int16_t>(6719), &mask, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008,
                                   1009, 1010, 1011);

  EXPECT_EQ(g_call.maskSeen, 0x0807u) << "the original did not see allow_incoming forced on";
  EXPECT_EQ(mask, 0x0807u) << "the caller's mask was not updated in place";
  EXPECT_EQ(g_call.self, &self);
  EXPECT_EQ(g_call.nameHash, 0xABCDu);
  EXPECT_EQ(g_call.port, static_cast<std::int16_t>(6719));
  for (std::size_t i = 0; i < 11; ++i) {
    EXPECT_EQ(g_call.stack[i], 1001u + i) << "stack argument p" << (5 + i) << " was not forwarded";
  }
  EXPECT_EQ(ret, kStandInReturn) << "the original's return value did not come back unchanged";

  // A second call with the bit already set: the detour leaves the mask as it is and still forwards everything.
  mask = 0x0807u;
  target(&self, 0xABCDu, static_cast<std::int16_t>(6719), &mask, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011);
  EXPECT_EQ(g_call.maskSeen, 0x0807u);
  EXPECT_EQ(mask, 0x0807u);
}

}  // namespace
