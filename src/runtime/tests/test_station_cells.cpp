// #600: the equipment station's grid hooks (runtime/combat/station_cells.h): the decisions, and the two hooks installed
// through the script hook table under MinHook on a fake station script.
//
// The fake script is one RWX allocation with the real first bytes of the count (0x16710) and newposition (0x1D130) at
// their RVAs and short bodies written here: the count returns 3 (the stock min), newposition writes (1.0, 2.0) to its
// first argument and returns it. The context is laid out as the script's: ctx[0] -> +0x68 -> the script data with
// max_icons at +0xB0, and ctx[0x19] -> the thread data { gear type, cell }.
//
// NOT shown by these tests: the shipped script's bytes in a run, and the station redrawn in a match after LOADOUTS.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/station_cells.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace sc = nevr_combat::station_cells;
namespace sh = nevr_script_hooks;

constexpr std::int32_t kStockCount = 3;
constexpr std::int32_t kMaxIcons = 8;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

TEST(StationCellsDecision, OnlyAPublishedTypeChanges) {
  std::uint8_t counts[3] = {0, 4, 0};
  EXPECT_EQ(sc::Published(counts, 1), 4);
  EXPECT_EQ(sc::Published(counts, 0), 0);
  EXPECT_EQ(sc::Published(counts, -1), 0) << "not a station gear type";
  EXPECT_EQ(sc::Published(counts, 3), 0) << "not a station gear type";
  EXPECT_EQ(sc::Published(nullptr, 1), 0);
  EXPECT_EQ(sc::Count(kStockCount, kMaxIcons, 1, counts), kMaxIcons) << "published: every cell";
  EXPECT_EQ(sc::Count(kStockCount, kMaxIcons, 0, counts), kStockCount) << "not published: the game's count";
  EXPECT_TRUE(sc::OffCanvas(1, 4, counts)) << "the first cell past the count";
  EXPECT_TRUE(sc::OffCanvas(1, 7, counts));
  EXPECT_FALSE(sc::OffCanvas(1, 3, counts)) << "the last cell in use";
  EXPECT_FALSE(sc::OffCanvas(0, 7, counts)) << "not published: stock";
}

class StationCellsInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    sc::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    std::vector<std::uint8_t> count(sc::kCountPrologue, sc::kCountPrologue + sizeof sc::kCountPrologue);
    count.insert(count.end(), {0xb8, kStockCount, 0, 0, 0, 0x5f, 0xc3});  // mov eax, 3; pop rdi; ret
    std::memcpy(base_ + sc::kCountRva, count.data(), count.size());
    std::vector<std::uint8_t> pos(sc::kPositionPrologue, sc::kPositionPrologue + sizeof sc::kPositionPrologue);
    const float stock[2] = {1.0F, 2.0F};
    pos.insert(pos.end(), {0x48, 0xb8});                                 // mov rax, (1.0, 2.0)
    const auto* s = reinterpret_cast<const std::uint8_t*>(stock);
    pos.insert(pos.end(), s, s + sizeof stock);
    pos.insert(pos.end(), {0x48, 0x89, 0x01,                             // mov [rcx], rax
                           0x48, 0x89, 0xc8,                             // mov rax, rcx
                           0x48, 0x83, 0xc4, 0x70, 0x5f, 0xc3});         // add rsp, 0x70; pop rdi; ret
    std::memcpy(base_ + sc::kPositionRva, pos.data(), pos.size());

    std::memcpy(data_ + sc::kMaxIconsOffset, &kMaxIcons, sizeof kMaxIcons);
    void* data = data_;
    std::memcpy(holder_ + 0x68, &data, sizeof data);
    ctx_[0] = reinterpret_cast<std::int64_t>(holder_);
    ctx_[0x19] = reinterpret_cast<std::int64_t>(thread_);
  }
  void TearDown() override {
    MH_RemoveHook(base_ + sc::kCountRva);
    MH_RemoveHook(base_ + sc::kPositionRva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    sc::ResetForTest();
    SetBootLogSink(nullptr);
  }
  std::int32_t RunCount(std::int32_t type) {
    thread_[0] = type;
    return reinterpret_cast<std::int32_t (*)(std::int64_t*)>(base_ + sc::kCountRva)(ctx_);
  }
  std::pair<float, float> RunPosition(std::int32_t type, std::int32_t cell) {
    thread_[0] = type;
    thread_[1] = cell;
    float out[2] = {0, 0};
    auto* r = reinterpret_cast<std::uint64_t* (*)(std::uint64_t*, std::int64_t*)>(base_ + sc::kPositionRva)(
        reinterpret_cast<std::uint64_t*>(out), ctx_);
    EXPECT_EQ(reinterpret_cast<void*>(r), static_cast<void*>(out)) << "newposition returns its argument";
    return {out[0], out[1]};
  }
  std::uint8_t* base_ = nullptr;
  std::uint8_t counts_[3] = {};
  alignas(8) std::uint8_t data_[0x100] = {};
  alignas(8) std::uint8_t holder_[0x80] = {};
  std::int32_t thread_[2] = {};
  std::int64_t ctx_[0x20] = {};
};

TEST_F(StationCellsInstalled, AShrunkColumnLeavesNoIconBehind) {
  sc::Register(counts_);
  sh::OnLoaded(sc::kScript, base_);
  EXPECT_EQ(Count("script hooks installed script=e0fdfda9a944e8ff"), 1);
  EXPECT_EQ(Count("hooks=2"), 2) << "the registration line and the install line";
  // Nothing published: the stock layout.
  EXPECT_EQ(RunCount(1), kStockCount);
  EXPECT_EQ(RunPosition(1, 6), std::make_pair(1.0F, 2.0F));
  // Weapons (type 1) published at 4 cells: every cell is positioned, the ones past 4 off the canvas.
  counts_[1] = 4;
  EXPECT_EQ(RunCount(1), kMaxIcons);
  EXPECT_EQ(RunPosition(1, 3), std::make_pair(1.0F, 2.0F)) << "a cell in use keeps the game's position";
  EXPECT_EQ(RunPosition(1, 4), std::make_pair(sc::kFar, sc::kFar));
  EXPECT_EQ(RunPosition(1, 7), std::make_pair(sc::kFar, sc::kFar));
  // Another type, not published, is untouched.
  EXPECT_EQ(RunCount(0), kStockCount);
  EXPECT_EQ(RunPosition(0, 7), std::make_pair(1.0F, 2.0F));

  sh::OnUnloading(sc::kScript, base_);
  EXPECT_EQ(RunCount(1), kStockCount) << "unhooked with the script";
  EXPECT_EQ(RunPosition(1, 7), std::make_pair(1.0F, 2.0F));
}

TEST_F(StationCellsInstalled, NoThreadDataLeavesTheGamesResult) {
  sc::Register(counts_);
  sh::OnLoaded(sc::kScript, base_);
  counts_[1] = 4;
  ctx_[0x19] = 0;
  EXPECT_EQ(reinterpret_cast<std::int32_t (*)(std::int64_t*)>(base_ + sc::kCountRva)(ctx_), kStockCount);
  float out[2] = {0, 0};
  reinterpret_cast<std::uint64_t* (*)(std::uint64_t*, std::int64_t*)>(base_ + sc::kPositionRva)(
      reinterpret_cast<std::uint64_t*>(out), ctx_);
  EXPECT_EQ(out[0], 1.0F);
}

TEST_F(StationCellsInstalled, RegisteringTwiceAddsTheRowsOnce) {
  sc::Register(counts_);
  sc::Register(counts_);
  sh::OnLoaded(sc::kScript, base_);
  EXPECT_EQ(Count("station cells fix registered"), 1);
  EXPECT_EQ(Count("script hooks installed script=e0fdfda9a944e8ff base="), 1);
  EXPECT_EQ(Count("hooks=2 slots=0"), 1);
}

}  // namespace
