// The combat module's write gate refuses a write that overlaps a range the runtime hooked or wrote
// (runtime/combat/write_gate.h), against the real HookGuard and the real ProcessMemcpy.

#include <winsock2.h>
#include <windows.h>

#include <cstdint>

#include <gtest/gtest.h>

#include "runtime/combat/write_gate.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/process_memory.h"
#include "runtime/hook/write_ranges.h"

namespace {

class CombatWriteGate : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_hook_guard::ResetForTest();
    nevr_write_ranges::ResetForTest();
    page_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ASSERT_NE(page_, nullptr);
  }
  void TearDown() override {
    nevr_hook_guard::ResetForTest();
    nevr_write_ranges::ResetForTest();
    VirtualFree(page_, 0, MEM_RELEASE);
  }
  std::uint8_t* page_ = nullptr;
};

TEST_F(CombatWriteGate, AWriteOverlappingARecordedHookIsRefused) {
  nevr_hook_guard::Record(page_ + 0x100, "TestHook");
  EXPECT_FALSE(nevr_combat::GateWrite(page_ + 0x108, 4, "inside the hook's 16 bytes"));
  EXPECT_FALSE(nevr_combat::GateWrite(page_ + 0xF0, 0x11, "last byte on the hook's first"));
  EXPECT_FALSE(nevr_combat::GateWrite(page_ + 0x10F, 1, "the hook's last byte"));
}

TEST_F(CombatWriteGate, AWriteBesideARecordedHookIsAllowed) {
  nevr_hook_guard::Record(page_ + 0x100, "TestHook");
  EXPECT_TRUE(nevr_combat::GateWrite(page_ + 0xF0, 0x10, "ends where the hook starts"));
  EXPECT_TRUE(nevr_combat::GateWrite(page_ + 0x110, 8, "starts where the hook ends"));
}

TEST_F(CombatWriteGate, AWriteOverlappingARangeProcessMemcpyWroteIsRefused) {
  const std::uint8_t bytes[8] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
  ASSERT_TRUE(ProcessMemcpy(page_ + 0x200, bytes, sizeof(bytes), nullptr, "TestPatch"));
  EXPECT_FALSE(nevr_combat::GateWrite(page_ + 0x204, 2, "inside the patch"));
  EXPECT_TRUE(nevr_combat::GateWrite(page_ + 0x208, 2, "after the patch"));
}

TEST_F(CombatWriteGate, OverlapNamesTheRuntimeRange) {
  nevr_hook_guard::Record(page_ + 0x300, "TestHook");
  nevr_hook_guard::Overlap hit{};
  ASSERT_TRUE(nevr_hook_guard::FindOverlap(page_ + 0x30A, 4, &hit));
  EXPECT_EQ(hit.start, reinterpret_cast<std::uintptr_t>(page_ + 0x300));
  EXPECT_EQ(hit.length, 16u);
  EXPECT_STREQ(hit.name, "TestHook");
  EXPECT_STREQ(hit.kind, "hook");
}

TEST_F(CombatWriteGate, NothingRecordedAllowsTheWrite) {
  EXPECT_TRUE(nevr_combat::GateWrite(page_ + 0x400, 0x2c0, "Cat's .data-tail cells"));
}

}  // namespace
