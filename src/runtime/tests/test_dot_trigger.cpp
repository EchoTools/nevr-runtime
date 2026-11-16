// #2242 (H-DOTT): the DOT FIELD remote trigger script's two hooks (runtime/combat/dot_trigger.h), through the script hook
// table and MinHook, on a fake copy of the detonator script.
//
// The fake script has the real prologue bytes of the event dispatcher (FUN_18000B870) and of resolve_next_node
// (0xBD00) at their RVAs. The dispatcher's body is `mov rax,[rdx]; ret`, so it returns the event it was handed; the
// next-node body returns 0x77 for every node. The decisions are tested without a fake image.
//
// NOT shown by these tests: the copy's bytes in a run, the sounds or the trigger hand in a match, or that the game's
// event values are the ones this table names (the plan's table, checked against the detonator's compare constants only).

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
#include "runtime/combat/dot_trigger.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace dt = nevr_combat::dot_trigger;
namespace sh = nevr_script_hooks;

constexpr std::uint64_t kStock = 0x1234;
constexpr std::uint64_t kOtherEvent = 0x33f5911416f09172ULL;  // a row's event, not a trigger event
constexpr std::uint64_t kNotARow = 0x12345678ULL;
constexpr std::uint64_t kFakeNextFromStock = 0x77;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

// --- decisions (no fake image) ---

TEST(DotTriggerDecision, TheThreeEventsAreTheOnesTheOriginalNeverSees) {
  EXPECT_TRUE(dt::IsTriggerEvent(0x0360ce7dfc51dd43ULL)) << "delegate_onspawnedremote";
  EXPECT_TRUE(dt::IsTriggerEvent(0x088725a83299f252ULL)) << "delegate_ondeposited";
  EXPECT_TRUE(dt::IsTriggerEvent(0x71b428190a37733eULL)) << "delegate_ontypechanged";
  EXPECT_FALSE(dt::IsTriggerEvent(kOtherEvent));
  EXPECT_FALSE(dt::IsTriggerEvent(0)) << "event 0 is not a trigger event";
  EXPECT_FALSE(dt::IsTriggerEvent(dt::kNoEvent)) << "the replacement id is not itself a trigger";
}

TEST(DotTriggerDecision, AnEventTheOriginalSeesIsTheReplacementForATriggerAndItselfOtherwise) {
  EXPECT_EQ(dt::EventForOriginal(0x0360ce7dfc51dd43ULL), dt::kNoEvent);
  EXPECT_EQ(dt::EventForOriginal(0x088725a83299f252ULL), dt::kNoEvent);
  EXPECT_EQ(dt::EventForOriginal(0x71b428190a37733eULL), dt::kNoEvent);
  EXPECT_EQ(dt::EventForOriginal(kOtherEvent), kOtherEvent);
  EXPECT_EQ(dt::EventForOriginal(0), 0u);
}

TEST(DotTriggerDecision, TheRowsReturnTheirNextNodeAndAnotherPairKeepsTheStockOne) {
  EXPECT_EQ(dt::NextNode(0x75014b69, 0x33f5911416f09172ULL, kStock), dt::kStop);
  EXPECT_EQ(dt::NextNode(0x75014b69, 0, kStock), dt::kStop) << "the row also matches event 0";
  EXPECT_EQ(dt::NextNode(0x75014b69, 5, kStock), kStock) << "event 5 is not one of its events";
  EXPECT_EQ(dt::NextNode(0xf8ab569b, 0x33f5911416f09172ULL, kStock), dt::kStop);
  EXPECT_EQ(dt::NextNode(0xf8ab569b, 0, kStock), dt::kStop);
  EXPECT_EQ(dt::NextNode(0x8935bc5b, 0x56ccd1bd99a59d3bULL, kStock), dt::kStop);
  EXPECT_EQ(dt::NextNode(0x8935bc5b, 0, kStock), dt::kStop);
  EXPECT_EQ(dt::NextNode(0xb0c42820, 0xe32dc7dfd49f50a9ULL, kStock), 0x5a04a5e5u);
  EXPECT_EQ(dt::NextNode(0xb0c42820, 0x33f5841b26fb8162ULL, kStock), 0x5a04a5e5u);
  EXPECT_EQ(dt::NextNode(0xb61a3584, 0x71b428190a37733eULL, kStock), 0x5a04a5e5u);
  EXPECT_EQ(dt::NextNode(0xb0c42820, 0, kStock), kStock) << "rows 4 and 5 do not match event 0";
  EXPECT_EQ(dt::NextNode(0xb61a3584, 0, kStock), kStock) << "row 6 does not match event 0";
  EXPECT_EQ(dt::NextNode(kNotARow, kOtherEvent, kStock), kStock) << "another node is the game's";
}

TEST(DotTriggerDecision, NodeZeroIsNoRowSoItKeepsTheStockNodeForEveryEvent) {
  // The table is six rows; a zero-initialised trailing row would match (node 0, event 0) and return 0, not the game's node.
  EXPECT_EQ(dt::NextNode(0, 0, kStock), kStock);
  EXPECT_EQ(dt::NextNode(0, kOtherEvent, kStock), kStock);
}

// --- installed on a fake image ---

class DotTriggerInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    dt::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x12000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);

    // Dispatcher: its real first 17 bytes (the 16-byte prologue, then the movabs's last byte is not reached: the body starts
    // after it), then the fake body. The prologue pushes rbx and rdi and subtracts 0x28 from rsp, so the body undoes them
    // (add rsp,0x28; pop rdi; pop rbx) before it returns the event it was handed (mov rax,[rdx]; ret).
    std::vector<std::uint8_t> dispatch(dt::kDispatchPrologue, dt::kDispatchPrologue + sizeof dt::kDispatchPrologue);
    dispatch.push_back(0xaa);
    dispatch.insert(dispatch.end(), {0x48, 0x83, 0xc4, 0x28, 0x5f, 0x5b, 0x48, 0x8b, 0x02, 0xc3});
    std::memcpy(base_ + dt::kDispatchRva, dispatch.data(), dispatch.size());

    // resolve_next_node: its real 10-byte prologue, the jg's operand, then `mov eax, 0x77; ret`.
    std::vector<std::uint8_t> next(dt::kNextNodePrologue, dt::kNextNodePrologue + sizeof dt::kNextNodePrologue);
    next.insert(next.end(), {0x00, 0x00, 0x00, 0x00, 0xb8, 0x77, 0x00, 0x00, 0x00, 0xc3});
    std::memcpy(base_ + dt::kNextNodeRva, next.data(), next.size());
  }
  void TearDown() override {
    MH_RemoveHook(base_ + dt::kDispatchRva);
    MH_RemoveHook(base_ + dt::kNextNodeRva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    dt::ResetForTest();
    SetBootLogSink(nullptr);
  }
  std::uint64_t Dispatch(std::uint64_t event) {
    std::uint64_t value = event;
    return reinterpret_cast<std::uint64_t (*)(void*, std::uint64_t*)>(base_ + dt::kDispatchRva)(nullptr, &value);
  }
  std::uint64_t Next(std::uint64_t node, std::uint64_t event) {
    return reinterpret_cast<std::uint64_t (*)(std::uint64_t, std::uint64_t)>(base_ + dt::kNextNodeRva)(node, event);
  }
  void Load() {
    dt::Register();
    sh::OnLoaded(dt::kScript, base_);
  }
  std::uint8_t* base_ = nullptr;
};

TEST_F(DotTriggerInstalled, NoneOfTheThreeTriggerEventsReachesTheOriginalAndEveryOtherEventDoes) {
  EXPECT_EQ(Dispatch(0x0360ce7dfc51dd43ULL), 0x0360ce7dfc51dd43ULL) << "stock: the original sees the event";
  Load();
  EXPECT_EQ(Count("script hooks installed script=4e8af51fe7d6af8c"), 1);
  EXPECT_EQ(Dispatch(0x0360ce7dfc51dd43ULL), dt::kNoEvent) << "delegate_onspawnedremote";
  EXPECT_EQ(Dispatch(0x088725a83299f252ULL), dt::kNoEvent) << "delegate_ondeposited";
  EXPECT_EQ(Dispatch(0x71b428190a37733eULL), dt::kNoEvent) << "delegate_ontypechanged";
  EXPECT_EQ(Dispatch(kOtherEvent), kOtherEvent) << "another event passes through";
  EXPECT_EQ(Dispatch(0), 0u) << "event 0 passes through";
}

TEST_F(DotTriggerInstalled, TheTableRowsReturnTheirNextNodeThroughTheHook) {
  Load();
  EXPECT_EQ(Next(0x75014b69, 0x33f5911416f09172ULL), dt::kStop);
  EXPECT_EQ(Next(0x75014b69, 0), dt::kStop);
  EXPECT_EQ(Next(0xf8ab569b, 0x33f5911416f09172ULL), dt::kStop);
  EXPECT_EQ(Next(0x8935bc5b, 0x56ccd1bd99a59d3bULL), dt::kStop);
  EXPECT_EQ(Next(0xb0c42820, 0xe32dc7dfd49f50a9ULL), 0x5a04a5e5u);
  EXPECT_EQ(Next(0xb61a3584, 0x71b428190a37733eULL), 0x5a04a5e5u);
  EXPECT_EQ(Next(0xb0c42820, 0), kFakeNextFromStock) << "a row's node with another event is the game's";
  EXPECT_EQ(Next(kNotARow, kOtherEvent), kFakeNextFromStock) << "another node is the game's";
}

TEST_F(DotTriggerInstalled, UnloadingRemovesBothHooks) {
  Load();
  EXPECT_EQ(Dispatch(0x088725a83299f252ULL), dt::kNoEvent);
  EXPECT_EQ(Next(0x75014b69, 0), dt::kStop);
  sh::OnUnloading(dt::kScript, base_);
  EXPECT_EQ(Dispatch(0x088725a83299f252ULL), 0x088725a83299f252ULL) << "the dispatcher is the game's again";
  EXPECT_EQ(Next(0x75014b69, 0), kFakeNextFromStock) << "resolve_next_node is the game's again";
}

TEST_F(DotTriggerInstalled, RegisteringTwiceAddsTheRowsOnceAndSaysSo) {
  dt::Register();
  dt::Register();
  sh::OnLoaded(dt::kScript, base_);
  EXPECT_EQ(Count("DOT trigger fix registered script=4e8af51fe7d6af8c hooks=2"), 1);
}

}  // namespace
