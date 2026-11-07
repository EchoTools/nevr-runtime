// #600: the script guards (runtime/combat/script_guard.h): the decision, and the heal guard installed through the script
// hook table (runtime/hook/script_hooks.h) on a fake heal script under MinHook.
//
// The fake heal script is one RWX allocation with the stock dispatcher's first bytes at its real RVA (0xA200), then
// `mov eax, 7; ret`: a node the guard lets through returns 7, a node it keeps returns 0 (the dispatcher's "node done").
//
// NOT shown by these tests: the shipped heal script's bytes in a run, and that a skipped activation leaves the game as his
// patch did (the PR's live run).

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
#include "runtime/combat/equip_cell.h"
#include "runtime/combat/script_guard.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace sg = nevr_combat::script_guard;
namespace sh = nevr_script_hooks;

constexpr std::uint64_t kBuff = 0xc8c33e48386618b6ULL;  // CSymbol64("buff")
constexpr std::int64_t kOtherNode = 0x80d6ac28;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

const sg::Guard& Heal() {
  std::size_t n = 0;
  return sg::Guards(&n)[0];
}

TEST(ScriptGuardDecision, OnlyAClientsGuardedNodeForAnotherSelectedTacModIsKept) {
  const sg::Guard& g = Heal();
  EXPECT_TRUE(sg::Skips(g, false, kBuff, sg::kHealBubbleActivate));
  EXPECT_FALSE(sg::Skips(g, true, kBuff, sg::kHealBubbleActivate)) << "a server runs every node";
  EXPECT_FALSE(sg::Skips(g, false, 0, sg::kHealBubbleActivate)) << "no selection yet: stock";
  EXPECT_FALSE(sg::Skips(g, false, sg::kHealSymbol, sg::kHealBubbleActivate)) << "the script's own tac mod";
  EXPECT_FALSE(sg::Skips(g, false, kBuff, kOtherNode)) << "an unguarded node";
}

TEST(ScriptGuardDecision, TheHealGuardIsTheStockHealScriptsDispatcher) {
  const sg::Guard& g = Heal();
  EXPECT_EQ(g.script, 0x6a0f689e5b91c666ULL);
  EXPECT_EQ(g.dispatch_rva, 0xA200u);
  EXPECT_EQ(g.own, 0xc8c33e4832761fbcULL);
  ASSERT_EQ(g.node_count, 1u);
  EXPECT_EQ(g.nodes[0], 0xfb9e3f50);
}

class ScriptGuardInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    sg::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0xB000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    std::memcpy(base_ + 0xA200, sg::kHealDispatchPrologue, sizeof sg::kHealDispatchPrologue);
    const std::uint8_t body[] = {0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3};  // mov eax, 7; ret
    std::memcpy(base_ + 0xA200 + sizeof sg::kHealDispatchPrologue, body, sizeof body);
    std::memset(cell_, 0, sizeof cell_);
  }
  void TearDown() override {
    MH_RemoveHook(base_ + 0xA200);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    sg::ResetForTest();
    SetBootLogSink(nullptr);
  }
  std::uint64_t Run(std::int64_t node) {
    std::int64_t thread[4] = {0, 0, node, 0};
    return reinterpret_cast<std::uint64_t (*)(std::int64_t*)>(base_ + 0xA200)(thread);
  }
  std::uint32_t CellSkips() const {
    std::uint32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(cell_) + nevr_combat::kEquipSkipsOffset, sizeof v);
    return v;
  }
  std::uint8_t* base_ = nullptr;
  std::uint64_t cell_[6] = {};
};

TEST_F(ScriptGuardInstalled, TheHealActivationRunsOnlyForHeal) {
  sg::Register(cell_, /*is_server=*/false);
  sh::OnLoaded(sg::kHealScript, base_);
  EXPECT_EQ(Count("script hooks installed script=6a0f689e5b91c666"), 1);
  EXPECT_EQ(Run(sg::kHealBubbleActivate), 7u) << "nothing selected yet";
  cell_[0] = sg::kHealSymbol;
  EXPECT_EQ(Run(sg::kHealBubbleActivate), 7u) << "heal selected";
  cell_[0] = kBuff;
  EXPECT_EQ(Run(sg::kHealBubbleActivate), 0u) << "OVERCHARGE selected: heal's activation is kept";
  EXPECT_EQ(Run(kOtherNode), 7u) << "heal's other nodes still run";
  EXPECT_EQ(sg::SkipsCounted(0), 1u);
  EXPECT_EQ(CellSkips(), 1u) << "counted where VeilNet reports skips";
  EXPECT_EQ(Count("script node skipped guard=\"heal (Repair Matrix)\""), 1);
  sh::OnUnloading(sg::kHealScript, base_);
  EXPECT_EQ(Run(sg::kHealBubbleActivate), 7u) << "unhooked with the script";
}

TEST_F(ScriptGuardInstalled, AServerRunsTheActivationWhateverIsSelected) {
  sg::Register(cell_, /*is_server=*/true);
  sh::OnLoaded(sg::kHealScript, base_);
  cell_[0] = kBuff;
  EXPECT_EQ(Run(sg::kHealBubbleActivate), 7u);
  EXPECT_EQ(CellSkips(), 0u);
  EXPECT_EQ(Count("script guards registered guards=1 server=true"), 1);
}

TEST_F(ScriptGuardInstalled, RegisteringTwiceAddsTheRowsOnce) {
  sg::Register(cell_, false);
  sg::Register(cell_, false);
  sh::OnLoaded(sg::kHealScript, base_);
  EXPECT_EQ(Count("hooks=1"), 1);
  EXPECT_EQ(Count("script guards registered"), 1);
}

}  // namespace
