// PLAN-v7 H-CTRL (#2242): the gear controller's chain decisions (runtime/combat/gear_controller.h) on a fake equip cell.
//
// Pure: no game image, no hook, no MinHook. The cell is a 0x40-byte buffer with the words at the offsets of equip_cell.h
// (+00 selected, +08 writer, +10 skips, +18 queried, +20 re-runs); each test writes the words it needs and reads back what the
// decision wrote. The constants are pinned here against his tools (fix_smoke.py, equip_guard.py) and the symbol hash, not read
// from the module under test.
//
// NOT shown by these tests: that the controller's chain runs in a game (no cave is written; see the header's NOT DONE list),
// that the smoke node enables its copy, and the controller's per-node call rate (ADR 0038 has no hook here to measure).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "nevr/symbol_hash.h"
#include "runtime/combat/equip_cell.h"
#include "runtime/combat/gear_controller.h"

namespace {

namespace gc = nevr_combat::gear_controller;

// Pinned from his tools (fix_smoke.py:99-101, equip_guard.py:49-52) and equip_cell.h, written once here.
constexpr std::int64_t kPinnedWraithNode = 0x5613C5A8;
constexpr std::int64_t kPinnedSmokeNode = 0x5613C5A9;
constexpr std::int64_t kPinnedQueryNode = 0x52A8D314;
constexpr std::uint64_t kPinnedOutPort = 0x33F5911416F09172ULL;
constexpr std::uint32_t kPinnedMaxReruns = 3;
constexpr std::uint64_t kSelectedSmoke = 0xE32DC7C9D4804FB5ULL;
constexpr std::uint64_t kSelectedBuff = 0xC8C33E48386618B6ULL;
constexpr std::uint64_t kOtherPort = 1;
constexpr std::uint32_t kCellSize = 0x40;

// The smoke symbol is CSymbol64("smoke"): the constant and the hash agree.
static_assert(EchoVR::CSymbol64Hash("smoke") == gc::kSmokeSymbol, "kSmokeSymbol is CSymbol64(\"smoke\")");
static_assert(EchoVR::CSymbol64Hash("smoke") == kSelectedSmoke, "the pinned smoke symbol is CSymbol64(\"smoke\")");
static_assert(gc::kSmokeNode == gc::kWraithNode + 1, "the smoke node is the node after wraith (fix_smoke.py NEW_NODE)");

using Cell = std::array<std::uint8_t, kCellSize>;

// A cell whose every byte is `fill`, so a write that strays into another word shows as a changed byte.
Cell FilledCell(std::uint8_t fill) {
  Cell cell{};
  cell.fill(fill);
  return cell;
}

void PutU64(Cell& cell, std::size_t offset, std::uint64_t value) { std::memcpy(cell.data() + offset, &value, sizeof value); }
void PutU32(Cell& cell, std::size_t offset, std::uint32_t value) { std::memcpy(cell.data() + offset, &value, sizeof value); }
std::uint64_t GetU64(const Cell& cell, std::size_t offset) {
  std::uint64_t value = 0;
  std::memcpy(&value, cell.data() + offset, sizeof value);
  return value;
}
std::uint32_t GetU32(const Cell& cell, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, cell.data() + offset, sizeof value);
  return value;
}

TEST(GearControllerConstants, MatchHisToolsAndTheCellLayout) {
  EXPECT_EQ(gc::kWraithNode, kPinnedWraithNode);
  EXPECT_EQ(gc::kSmokeNode, kPinnedSmokeNode);
  EXPECT_EQ(gc::kQueryNode, kPinnedQueryNode);
  EXPECT_EQ(gc::kOutPort, kPinnedOutPort);
  EXPECT_EQ(gc::kDefaultPort, 0u);
  EXPECT_EQ(gc::kEnd, -1);
  EXPECT_EQ(gc::kMaxReruns, kPinnedMaxReruns);
  // The cell's words: the chain's offsets are equip_cell.h's (+00, +08, +18, +20), and the skip count (+10) is not one of them.
  EXPECT_EQ(gc::kSelectedOffset, 0x00u);
  EXPECT_EQ(gc::kWriterOffset, 0x08u);
  EXPECT_EQ(gc::kQueriedOffset, 0x18u);
  EXPECT_EQ(gc::kRerunsOffset, 0x20u);
  EXPECT_EQ(nevr_combat::kEquipSkipsOffset, 0x10u);
}

TEST(GearControllerChain, RunsTheBranchesInTheOrderTheyWereAdded) {
  ASSERT_EQ(gc::kChainLength, 2u);
  EXPECT_EQ(gc::kChain[0], kPinnedWraithNode);
  EXPECT_EQ(gc::kChain[1], kPinnedSmokeNode);
  // Wraith's out-port 0 or the out-port continues to the smoke node; any other port ends the chain.
  EXPECT_EQ(gc::Successor(0, gc::kDefaultPort), kPinnedSmokeNode);
  EXPECT_EQ(gc::Successor(0, kPinnedOutPort), kPinnedSmokeNode);
  EXPECT_EQ(gc::Successor(0, kOtherPort), gc::kEnd);
  // The last node has no successor here: the tail decides it.
  EXPECT_EQ(gc::Successor(1, gc::kDefaultPort), gc::kEnd);
  EXPECT_EQ(gc::Successor(1, kPinnedOutPort), gc::kEnd);
}

TEST(GearControllerChain, EndsWhenNothingIsSelected) {
  // The tail's "none" check comes after the port check: no selection (0) ends the chain and leaves the count alone.
  const gc::TailStep step = gc::ChainTail(gc::kDefaultPort, 0, 5, 2);
  EXPECT_EQ(step.next, gc::kEnd);
  EXPECT_EQ(step.reruns, 2u);
}

TEST(GearControllerChain, EndsOnAnOtherPortWithoutCountingARerun) {
  const gc::TailStep step = gc::ChainTail(kOtherPort, kSelectedSmoke, kSelectedBuff, 1);
  EXPECT_EQ(step.next, gc::kEnd);
  EXPECT_EQ(step.reruns, 1u);
}

TEST(GearControllerChain, ResetsTheRerunCountWhenTheChainQueriedTheSelection) {
  // The chain already enabled the selected tac mod: end, and the count starts over for the next selection.
  const gc::TailStep step = gc::ChainTail(kPinnedOutPort, kSelectedSmoke, kSelectedSmoke, 2);
  EXPECT_EQ(step.next, gc::kEnd);
  EXPECT_EQ(step.reruns, 0u);
}

TEST(GearControllerChain, RerunsTheQueryAtMostThreeTimesPerSelection) {
  // Selected smoke, the chain queried buff: each tail re-runs the query and counts, the third re-run is the last allowed, and
  // the fourth ends the chain with the count at 4.
  std::uint32_t reruns = 0;
  gc::TailStep step = gc::ChainTail(gc::kDefaultPort, kSelectedSmoke, kSelectedBuff, reruns);
  EXPECT_EQ(step.next, kPinnedQueryNode);
  EXPECT_EQ(step.reruns, 1u);
  reruns = step.reruns;
  step = gc::ChainTail(gc::kDefaultPort, kSelectedSmoke, kSelectedBuff, reruns);
  EXPECT_EQ(step.next, kPinnedQueryNode);
  EXPECT_EQ(step.reruns, 2u);
  reruns = step.reruns;
  step = gc::ChainTail(gc::kDefaultPort, kSelectedSmoke, kSelectedBuff, reruns);
  EXPECT_EQ(step.next, kPinnedQueryNode);
  EXPECT_EQ(step.reruns, kPinnedMaxReruns);
  reruns = step.reruns;
  step = gc::ChainTail(gc::kDefaultPort, kSelectedSmoke, kSelectedBuff, reruns);
  EXPECT_EQ(step.next, gc::kEnd);
  EXPECT_EQ(step.reruns, kPinnedMaxReruns + 1);
}

TEST(GearControllerChain, TheCountSaturatesInsteadOfWrapping) {
  // A wrap to 0 would query again for a selection the chain has not enabled after 2^32 passes; the count stays just above the cap.
  const gc::TailStep step = gc::ChainTail(gc::kDefaultPort, kSelectedSmoke, kSelectedBuff, 0xFFFFFFFFu);
  EXPECT_EQ(step.next, gc::kEnd);
  EXPECT_EQ(step.reruns, kPinnedMaxReruns + 1);
}

TEST(GearControllerChain, RunTailReadsTheCellAndWritesTheCountBack) {
  Cell cell = FilledCell(0xAA);
  PutU64(cell, gc::kSelectedOffset, kSelectedSmoke);
  PutU64(cell, gc::kQueriedOffset, kSelectedBuff);
  PutU32(cell, gc::kRerunsOffset, 0);
  const Cell before = cell;

  const std::int64_t next = gc::RunTail(cell.data(), gc::kDefaultPort);

  EXPECT_EQ(next, kPinnedQueryNode);
  EXPECT_EQ(GetU32(cell, gc::kRerunsOffset), 1u);
  // Only the count's four bytes change: the selection, the queried tac mod and the skip count are left as they were.
  for (std::size_t i = 0; i < kCellSize; ++i) {
    if (i >= gc::kRerunsOffset && i < gc::kRerunsOffset + 4) continue;
    EXPECT_EQ(cell[i], before[i]) << "byte " << i << " changed";
  }
}

TEST(GearControllerCell, RecordQueryWritesTheQueriedTacModAndTheWriterAndNothingElse) {
  Cell cell = FilledCell(0xAA);
  constexpr std::uintptr_t kWriter = 0x7FF112233440;
  gc::RecordQuery(cell.data(), kSelectedBuff, kWriter);

  // +18 holds the queried tac mod, +08 the writer; the selection (+00), the writer's neighbour (+10, the skip count) and the
  // re-run count (+20) keep the fill.
  EXPECT_EQ(GetU64(cell, gc::kQueriedOffset), kSelectedBuff);
  EXPECT_EQ(GetU64(cell, gc::kWriterOffset), static_cast<std::uint64_t>(kWriter));
  EXPECT_EQ(GetU64(cell, gc::kSelectedOffset), 0xAAAAAAAAAAAAAAAAULL);
  EXPECT_EQ(GetU32(cell, nevr_combat::kEquipSkipsOffset), 0xAAAAAAAAu);
  EXPECT_EQ(GetU32(cell, gc::kRerunsOffset), 0xAAAAAAAAu);
}

}  // namespace
