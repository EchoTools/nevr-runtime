// #600: the script guards (runtime/combat/script_guard.h): the decision, the table of the four guarded scripts (the stock
// heal and his three copies), the held-script plan, and the guards installed through the script hook table
// (runtime/hook/script_hooks.h) on fake scripts under MinHook.
//
// The fake scripts are one RWX allocation holding each dispatcher's first bytes at its real RVA (0xA200 for the heal
// dispatcher, 0x5BE0 for the barrier's), then `mov eax, 7; ret`: a node the guard lets through returns 7, a node it keeps
// returns 0 (the dispatcher's "node done").
//
// NOT shown by these tests: the shipped scripts' bytes in a run, and that a skipped activation leaves the game as his
// patch did (the PR's live run).

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "nevr/symbol_hash.h"
#include "runtime/combat/equip_cell.h"
#include "runtime/combat/script_guard.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace sg = nevr_combat::script_guard;
namespace sh = nevr_script_hooks;

constexpr std::uint64_t kOtherSelection = 0x1234567890abcdefULL;  // a tac mod that is none of the four
constexpr std::int64_t kUnguardedNode = 0x80d6ac28;
constexpr std::uint32_t kHealRva = 0xA200;
constexpr std::uint32_t kBarrierRva = 0x5BE0;

// The four guards, in table order (script_guard.cpp kGuards).
constexpr std::size_t kTableSize = 4;
constexpr std::size_t kHealStock = 0;
constexpr std::size_t kBarrierSmokeCopy = 1;
constexpr std::size_t kBarrierPointCopy = 2;
constexpr std::size_t kOverchargeCopy = 3;

// The expected values, pinned here from the tree's own records (fix_smoke.py, fix_point.py, fix_buff.py, equip_guard.py),
// independently of the table under test. Each is written once, here.
constexpr std::uint64_t kPinnedHealScript = 0x6a0f689e5b91c666ULL;          // Repair Matrix, the stock heal
constexpr std::uint64_t kPinnedSmokeCopyScript = 0x94ba7ca09dcf7b0bULL;     // UMBRAL VEIL's barrier copy
constexpr std::uint64_t kPinnedPointCopyScript = 0x94ba7ca39fc97e1aULL;     // NEUTRON's barrier copy
constexpr std::uint64_t kPinnedOverchargeScript = 0x4b65d1b0088d2682ULL;    // OVERCHARGE's heal copy
constexpr std::uint64_t kUnknownScript = 0x1111111111111111ULL;             // a script no guard names

// The guards' own tac mod ids, pinned as CSymbol64 hashes of their names (see the static_asserts below).
constexpr std::uint64_t kPinnedHealSymbol = 0xc8c33e4832761fbcULL;   // "heal"
constexpr std::uint64_t kPinnedSmokeSymbol = 0xe32dc7c9d4804fb5ULL;  // "smoke"
constexpr std::uint64_t kPinnedPointSymbol = 0xe32dc7cad6864aa4ULL;  // "point"
constexpr std::uint64_t kPinnedBuffSymbol = 0xc8c33e48386618b6ULL;   // "buff"
constexpr std::size_t kHashHexDigits = 16;                           // a 64-bit hash as the log writes it (%016llx)
constexpr std::size_t kTwoHeldScripts = 2;                           // the held pair in the plan test

constexpr std::int64_t kPinnedCustomParamNode = 0xd11498f0;  // the barrier's custom-param use node
constexpr std::int64_t kPinnedWallCreateNode = 0x6fb98e2c;   // the barrier's wall create use node
constexpr std::int64_t kPinnedBubbleActivateNode = 0xfb9e3f50;  // the heal's bubble activate use node

constexpr std::size_t kBarrierNodeCount = 2;     // the barrier copies keep two use nodes
constexpr std::size_t kHealNodeCount = 1;        // the heal copy keeps one
constexpr std::size_t kDetourMinBytes = 5;       // a detour needs more than a jump's five bytes
constexpr std::uint64_t kSkipsAfterTwoCalls = 2;

// The fake image and the fake scripts' bodies.
constexpr std::size_t kFakeImageBytes = 0xB000;
constexpr std::uint8_t kOpcodeMovEaxImm32 = 0xb8;
constexpr std::uint8_t kOpcodeRet = 0xc3;
constexpr std::uint8_t kNodeRanByte = 7;      // what a node the guard lets through returns (`mov eax, 7`)
constexpr std::uint64_t kNodeRan = kNodeRanByte;
constexpr std::uint8_t kZeroByte = 0x00;
constexpr std::size_t kThreadWords = 4;       // the fake dispatcher's thread record
constexpr std::size_t kNodeIdSlot = 2;        // the node id's word in it (script_guard.cpp kNodeIdSlot)
constexpr std::size_t kCellWords = 6;         // the fake shared cell: the selection word, and the skips at its offset

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const std::string& needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

// A 64-bit hash as the log writes it (%016llx), so a test's expected text is built from the pinned value.
std::string Hex16(std::uint64_t v) {
  std::ostringstream out;
  out << std::hex << std::setw(static_cast<int>(kHashHexDigits)) << std::setfill('0') << v;
  return out.str();
}

const sg::Guard* Table(std::size_t* count) { return sg::Guards(count); }

// The guard's own tac mod is a CSymbol64 of its name: pinned against the hash, so a typo in the table fails here.
static_assert(EchoVR::CSymbol64Hash("heal") == kPinnedHealSymbol);
static_assert(EchoVR::CSymbol64Hash("smoke") == kPinnedSmokeSymbol);
static_assert(EchoVR::CSymbol64Hash("point") == kPinnedPointSymbol);
static_assert(EchoVR::CSymbol64Hash("buff") == kPinnedBuffSymbol);

TEST(ScriptGuardTable, TheTableHoldsTheStockHealAndCatsThreeCopiesInOrder) {
  std::size_t n = 0;
  const sg::Guard* g = Table(&n);
  ASSERT_EQ(n, kTableSize);
  EXPECT_EQ(g[kHealStock].script, kPinnedHealScript) << "Repair Matrix, the stock heal";
  EXPECT_EQ(g[kBarrierSmokeCopy].script, kPinnedSmokeCopyScript) << "UMBRAL VEIL's barrier copy (fix_smoke.py:377)";
  EXPECT_EQ(g[kBarrierPointCopy].script, kPinnedPointCopyScript) << "NEUTRON's barrier copy (fix_point.py:134)";
  EXPECT_EQ(g[kOverchargeCopy].script, kPinnedOverchargeScript) << "OVERCHARGE's heal copy (fix_buff.py:559)";
}

TEST(ScriptGuardTable, EachGuardsDispatcherIsItsScriptsOwnFunctionAtItsRva) {
  std::size_t n = 0;
  const sg::Guard* g = Table(&n);
  ASSERT_EQ(n, kTableSize);
  EXPECT_EQ(g[kHealStock].dispatch_rva, kHealRva);
  EXPECT_EQ(g[kBarrierSmokeCopy].dispatch_rva, kBarrierRva);
  EXPECT_EQ(g[kBarrierPointCopy].dispatch_rva, kBarrierRva);
  EXPECT_EQ(g[kOverchargeCopy].dispatch_rva, kHealRva);
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_NE(g[i].expected, nullptr) << i;
    EXPECT_GT(g[i].expected_len, kDetourMinBytes) << "the detour needs more than a jump's bytes: " << i;
  }
  EXPECT_EQ(g[kBarrierSmokeCopy].expected_len, sizeof sg::kBarrierDispatchPrologue);
  EXPECT_EQ(std::memcmp(g[kBarrierPointCopy].expected, sg::kBarrierDispatchPrologue, sizeof sg::kBarrierDispatchPrologue), 0);
}

TEST(ScriptGuardTable, EachCopyOwnsTheTacModItIsNamedFor) {
  std::size_t n = 0;
  const sg::Guard* g = Table(&n);
  ASSERT_EQ(n, kTableSize);
  EXPECT_EQ(g[kHealStock].own, EchoVR::CSymbol64Hash("heal"));
  EXPECT_EQ(g[kBarrierSmokeCopy].own, EchoVR::CSymbol64Hash("smoke"));
  EXPECT_EQ(g[kBarrierPointCopy].own, EchoVR::CSymbol64Hash("point"));
  EXPECT_EQ(g[kOverchargeCopy].own, EchoVR::CSymbol64Hash("buff"));
}

TEST(ScriptGuardTable, TheBarrierCopiesKeepTheCustomParamAndWallCreateNodesAndTheHealCopiesTheBubbleActivate) {
  std::size_t n = 0;
  const sg::Guard* g = Table(&n);
  ASSERT_EQ(n, kTableSize);
  ASSERT_EQ(g[kBarrierSmokeCopy].node_count, kBarrierNodeCount);
  EXPECT_EQ(g[kBarrierSmokeCopy].nodes[0], kPinnedCustomParamNode);
  EXPECT_EQ(g[kBarrierSmokeCopy].nodes[1], kPinnedWallCreateNode);
  ASSERT_EQ(g[kBarrierPointCopy].node_count, kBarrierNodeCount);
  EXPECT_EQ(g[kBarrierPointCopy].nodes[0], kPinnedCustomParamNode);
  EXPECT_EQ(g[kBarrierPointCopy].nodes[1], kPinnedWallCreateNode);
  ASSERT_EQ(g[kOverchargeCopy].node_count, kHealNodeCount);
  EXPECT_EQ(g[kOverchargeCopy].nodes[0], kPinnedBubbleActivateNode);
}

// Each copy keeps its nodes for another selection only: its own tac mod, nothing selected yet, a server, and a node it
// does not guard all let the game's dispatcher run.
TEST(ScriptGuardDecision, EachCopySkipsItsUseNodesOnlyForAnotherSelectedTacMod) {
  std::size_t n = 0;
  const sg::Guard* g = Table(&n);
  ASSERT_EQ(n, kTableSize);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t k = 0; k < g[i].node_count; ++k) {
      const std::int64_t node = g[i].nodes[k];
      EXPECT_TRUE(sg::Skips(g[i], false, kOtherSelection, node)) << "guard " << i << " keeps its node " << k;
      EXPECT_FALSE(sg::Skips(g[i], false, g[i].own, node)) << "guard " << i << ": its own tac mod runs it";
      EXPECT_FALSE(sg::Skips(g[i], false, 0, node)) << "guard " << i << ": nothing selected yet, stock";
      EXPECT_FALSE(sg::Skips(g[i], true, kOtherSelection, node)) << "guard " << i << ": a server runs every node";
    }
    EXPECT_FALSE(sg::Skips(g[i], false, kOtherSelection, kUnguardedNode)) << "guard " << i << ": an unguarded node";
  }
}

TEST(ScriptGuardDecision, TheBarrierCopiesKeepTheirNodesForEachOtherSelectionAlike) {
  std::size_t n = 0;
  const sg::Guard* g = Table(&n);
  ASSERT_EQ(n, kTableSize);
  const std::uint64_t smoke = g[kBarrierSmokeCopy].own;
  const std::uint64_t point = g[kBarrierPointCopy].own;
  EXPECT_TRUE(sg::Skips(g[kBarrierSmokeCopy], false, point, kPinnedWallCreateNode)) << "UMBRAL VEIL's wall, NEUTRON selected";
  EXPECT_FALSE(sg::Skips(g[kBarrierSmokeCopy], false, smoke, kPinnedWallCreateNode))
      << "UMBRAL VEIL's wall, UMBRAL VEIL selected";
  EXPECT_TRUE(sg::Skips(g[kBarrierPointCopy], false, smoke, kPinnedCustomParamNode))
      << "NEUTRON's custom param, UMBRAL VEIL selected";
  EXPECT_FALSE(sg::Skips(g[kBarrierPointCopy], false, point, kPinnedCustomParamNode))
      << "NEUTRON's custom param, NEUTRON selected";
}

TEST(ScriptGuardDecision, OverchargeIsKeptByItsOwnSelectionOnly) {
  std::size_t n = 0;
  const sg::Guard* g = Table(&n);
  ASSERT_EQ(n, kTableSize);
  const std::uint64_t smoke = g[kBarrierSmokeCopy].own;
  EXPECT_TRUE(sg::Skips(g[kOverchargeCopy], false, smoke, sg::kHealBubbleActivate)) << "UMBRAL VEIL selected: skipped";
  EXPECT_FALSE(sg::Skips(g[kOverchargeCopy], false, g[kOverchargeCopy].own, sg::kHealBubbleActivate)) << "OVERCHARGE runs";
  EXPECT_TRUE(sg::Skips(g[kHealStock], false, g[kOverchargeCopy].own, sg::kHealBubbleActivate))
      << "the stock heal keeps its activation for OVERCHARGE's selection (its own tac mod is heal)";
  EXPECT_FALSE(sg::Skips(g[kHealStock], false, g[kHealStock].own, sg::kHealBubbleActivate))
      << "the stock heal runs its activation for heal";
}

TEST(ScriptGuardPlan, PlanPicksExactlyTheGuardsWhoseScriptIsHeld) {
  const std::uint64_t smoke_copy[] = {kPinnedSmokeCopyScript};
  std::size_t out[kTableSize] = {};
  ASSERT_EQ(sg::Plan(smoke_copy, 1, out), 1u);
  EXPECT_EQ(out[0], kBarrierSmokeCopy) << "a held copy registers its rows";

  const std::uint64_t heal_and_overcharge[] = {kPinnedOverchargeScript, kPinnedHealScript};
  ASSERT_EQ(sg::Plan(heal_and_overcharge, kTwoHeldScripts, out), kTwoHeldScripts);
  EXPECT_EQ(out[0], kHealStock);
  EXPECT_EQ(out[1], kOverchargeCopy);

  EXPECT_EQ(sg::Plan(nullptr, 0, out), 0u) << "nothing held: nothing registers";
  const std::uint64_t unknown[] = {kUnknownScript};
  EXPECT_EQ(sg::Plan(unknown, 1, out), 0u) << "a script no guard names registers nothing";
}

TEST(ScriptGuardPlan, PlanKeepsTheTableOrderWhateverOrderTheHeldScriptsArrive) {
  const std::uint64_t held[] = {kPinnedOverchargeScript, kPinnedPointCopyScript, kPinnedHealScript, kPinnedSmokeCopyScript};
  std::size_t out[kTableSize] = {};
  ASSERT_EQ(sg::Plan(held, kTableSize, out), kTableSize);
  // The plan lists the held guards in table order: position k holds table index table_order[k].
  const std::size_t table_order[kTableSize] = {kHealStock, kBarrierSmokeCopy, kBarrierPointCopy, kOverchargeCopy};
  for (std::size_t k = 0; k < kTableSize; ++k) EXPECT_EQ(out[k], table_order[k]) << "plan position " << k;
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
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kFakeImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    // Each dispatcher: its prologue, then `mov eax, 7; ret`.
    const std::uint8_t body[] = {kOpcodeMovEaxImm32, kNodeRanByte, kZeroByte, kZeroByte, kZeroByte, kOpcodeRet};
    std::memcpy(base_ + kHealRva, sg::kHealDispatchPrologue, sizeof sg::kHealDispatchPrologue);
    std::memcpy(base_ + kHealRva + sizeof sg::kHealDispatchPrologue, body, sizeof body);
    std::memcpy(base_ + kBarrierRva, sg::kBarrierDispatchPrologue, sizeof sg::kBarrierDispatchPrologue);
    std::memcpy(base_ + kBarrierRva + sizeof sg::kBarrierDispatchPrologue, body, sizeof body);
    std::memset(cell_, 0, sizeof cell_);
  }
  void TearDown() override {
    MH_RemoveHook(base_ + kHealRva);
    MH_RemoveHook(base_ + kBarrierRva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    sg::ResetForTest();
    SetBootLogSink(nullptr);
  }
  std::uint64_t RunAt(std::uint32_t rva, std::int64_t node) {
    std::int64_t thread[kThreadWords] = {};
    thread[kNodeIdSlot] = node;
    return reinterpret_cast<std::uint64_t (*)(std::int64_t*)>(base_ + rva)(thread);
  }
  std::uint64_t Run(std::int64_t node) { return RunAt(kHealRva, node); }
  std::uint32_t CellSkips() const {
    std::uint32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(cell_) + nevr_combat::kEquipSkipsOffset, sizeof v);
    return v;
  }
  std::uint8_t* base_ = nullptr;
  std::uint64_t cell_[kCellWords] = {};
};

TEST_F(ScriptGuardInstalled, TheHealActivationRunsOnlyForHeal) {
  sg::Register(cell_, /*is_server=*/false);
  sh::OnLoaded(sg::kHealScript, base_);
  EXPECT_EQ(Count("script hooks installed script=" + Hex16(kPinnedHealScript)), 1);
  EXPECT_EQ(Run(sg::kHealBubbleActivate), kNodeRan) << "nothing selected yet";
  cell_[0] = sg::kHealSymbol;
  EXPECT_EQ(Run(sg::kHealBubbleActivate), kNodeRan) << "heal selected";
  cell_[0] = EchoVR::CSymbol64Hash("buff");
  EXPECT_EQ(Run(sg::kHealBubbleActivate), 0u) << "OVERCHARGE selected: heal's activation is kept";
  EXPECT_EQ(Run(kUnguardedNode), kNodeRan) << "heal's other nodes still run";
  EXPECT_EQ(sg::SkipsCounted(kHealStock), 1u);
  EXPECT_EQ(CellSkips(), 1u) << "counted where VeilNet reports skips";
  EXPECT_EQ(Count("script node skipped guard=\"heal (Repair Matrix)\""), 1);
  sh::OnUnloading(sg::kHealScript, base_);
  EXPECT_EQ(Run(sg::kHealBubbleActivate), kNodeRan) << "unhooked with the script";
}

TEST_F(ScriptGuardInstalled, AHeldBarrierCopyKeepsItsCustomParamForAnotherSelection) {
  const std::uint64_t held[] = {sg::kBarrierSmokeScript};
  sg::RegisterPlanned(cell_, /*is_server=*/false, held, 1);
  sh::OnLoaded(sg::kBarrierSmokeScript, base_);
  EXPECT_EQ(Count("script hooks installed script=" + Hex16(kPinnedSmokeCopyScript)), 1);
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedCustomParamNode), kNodeRan) << "nothing selected yet";
  cell_[0] = EchoVR::CSymbol64Hash("smoke");
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedCustomParamNode), kNodeRan) << "UMBRAL VEIL selected: its own custom param runs";
  cell_[0] = EchoVR::CSymbol64Hash("point");
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedCustomParamNode), 0u) << "NEUTRON selected: UMBRAL VEIL's custom param is kept";
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedWallCreateNode), 0u) << "and its wall create too";
  EXPECT_EQ(sg::SkipsCounted(kBarrierSmokeCopy), kSkipsAfterTwoCalls);
  EXPECT_EQ(CellSkips(), kSkipsAfterTwoCalls);
  sh::OnUnloading(sg::kBarrierSmokeScript, base_);
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedCustomParamNode), kNodeRan) << "unhooked with the script";
}

TEST_F(ScriptGuardInstalled, AHeldPointCopyKeepsItsCustomParamForAnotherSelection) {
  const std::uint64_t held[] = {sg::kBarrierPointScript};
  sg::RegisterPlanned(cell_, false, held, 1);
  sh::OnLoaded(sg::kBarrierPointScript, base_);
  cell_[0] = EchoVR::CSymbol64Hash("smoke");
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedCustomParamNode), 0u) << "UMBRAL VEIL selected: NEUTRON's custom param is kept";
  cell_[0] = EchoVR::CSymbol64Hash("point");
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedCustomParamNode), kNodeRan) << "NEUTRON selected: runs";
  EXPECT_EQ(sg::SkipsCounted(kBarrierPointCopy), 1u);
}

TEST_F(ScriptGuardInstalled, AHeldOverchargeCopyKeepsItsHealActivationForAnotherSelection) {
  const std::uint64_t held[] = {sg::kOverchargeScript};
  sg::RegisterPlanned(cell_, false, held, 1);
  sh::OnLoaded(sg::kOverchargeScript, base_);
  EXPECT_EQ(Count("script hooks installed script=" + Hex16(kPinnedOverchargeScript)), 1);
  cell_[0] = EchoVR::CSymbol64Hash("smoke");
  EXPECT_EQ(Run(sg::kHealBubbleActivate), 0u) << "UMBRAL VEIL selected: OVERCHARGE's activation is kept";
  cell_[0] = EchoVR::CSymbol64Hash("buff");
  EXPECT_EQ(Run(sg::kHealBubbleActivate), kNodeRan) << "OVERCHARGE selected: runs";
  EXPECT_EQ(sg::SkipsCounted(kOverchargeCopy), 1u);
}

TEST_F(ScriptGuardInstalled, ACopyThatIsNotHeldGetsNoRow) {
  const std::uint64_t held[] = {sg::kBarrierPointScript};
  sg::RegisterPlanned(cell_, false, held, 1);
  sh::OnLoaded(sg::kBarrierSmokeScript, base_);
  EXPECT_EQ(Count("script hooks installed script=" + Hex16(kPinnedSmokeCopyScript)), 0);
  cell_[0] = EchoVR::CSymbol64Hash("point");
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedCustomParamNode), kNodeRan) << "the smoke copy is not held: its dispatcher is stock";
  EXPECT_EQ(CellSkips(), 0u);
}

TEST_F(ScriptGuardInstalled, AServerRunsTheCopiesNodesWhateverIsSelected) {
  sg::Register(cell_, /*is_server=*/true);
  sh::OnLoaded(sg::kBarrierSmokeScript, base_);
  sh::OnLoaded(sg::kOverchargeScript, base_);
  cell_[0] = EchoVR::CSymbol64Hash("point");
  EXPECT_EQ(RunAt(kBarrierRva, kPinnedCustomParamNode), kNodeRan);
  EXPECT_EQ(CellSkips(), 0u);
  EXPECT_EQ(Count("script guards registered guards=" + std::to_string(kTableSize) + " server=true"), 1);
}

TEST_F(ScriptGuardInstalled, RegisteringTwiceAddsTheRowsOnce) {
  sg::Register(cell_, false);
  sg::Register(cell_, false);
  sh::OnLoaded(sg::kHealScript, base_);
  EXPECT_EQ(Count("hooks=1"), 1);
  EXPECT_EQ(Count("script guards registered"), 1);
}

}  // namespace
