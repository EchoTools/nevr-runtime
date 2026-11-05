// When the equipment change the game dropped is applied again (patch/equipment_refire_rules.h, #2121). The hook that feeds the
// rule is patch/equipment_refire.cpp (PC) and q15t/equiprefire.cpp (Quest); the game's setters, handlers and the local-player
// check need the game and are proven by a live run. The Quest host test compiles this same header.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>

#include "runtime/patch/equipment_refire_rules.h"

namespace {

using nevr_equipment_refire::ActorKey;
using nevr_equipment_refire::Debouncer;
using nevr_equipment_refire::kSettleMs;
using nevr_equipment_refire::Refire;
using nevr_equipment_refire::Slot;
using nevr_equipment_refire::Verdict;

constexpr std::uintptr_t kCs = 0x1000;
constexpr std::uint32_t kHandle = 0x00030002;
constexpr std::uint64_t kA = 0xA, kB = 0xB, kC = 0xC;
constexpr std::uint64_t kStart = 100000;

Verdict Local(Debouncer& d, Slot slot, std::uint64_t value, std::uint64_t now) {
  return d.Changed(slot, kCs, kHandle, value, /*local=*/true, /*server=*/false, /*changed=*/true, now);
}

TEST(EquipmentRefire, ThreeChangesWithin300msGiveOneRefireAfterTheSettleTimeWithTheLastValue) {
  Debouncer d;
  EXPECT_EQ(Local(d, Slot::kTacMod, kA, kStart), Verdict::kTracked);
  EXPECT_EQ(Local(d, Slot::kTacMod, kB, kStart + 150), Verdict::kTracked);
  EXPECT_EQ(Local(d, Slot::kTacMod, kC, kStart + 300), Verdict::kTracked);
  Refire r;
  EXPECT_FALSE(d.Due(kStart + 300 + kSettleMs - 1, &r)) << "the slot has not been still for the settle time";
  ASSERT_TRUE(d.Due(kStart + 300 + kSettleMs, &r));
  EXPECT_EQ(r.slot, Slot::kTacMod);
  EXPECT_EQ(r.owner, kCs);
  EXPECT_EQ(r.handle, kHandle);
  EXPECT_EQ(r.value, kC);
  EXPECT_EQ(r.changes, 3u);
  EXPECT_EQ(r.quietMs, kSettleMs);
  EXPECT_FALSE(d.Due(kStart + 300 + kSettleMs, &r)) << "one re-fire per quiet period";
  EXPECT_FALSE(d.Due(kStart + 100000, &r));
}

// The chain a change starts takes about 2 s (equipment_refire_rules.h); a shorter wait would send the event into the chain again.
TEST(EquipmentRefire, TheSettleTimeIsLongerThanTheChainItWaitsOut) {
  EXPECT_GT(kSettleMs, 2000u);
}

TEST(EquipmentRefire, EachChangeRestartsTheQuietPeriod) {
  Debouncer d;
  Local(d, Slot::kWeapon, kA, kStart);
  Local(d, Slot::kWeapon, kB, kStart + kSettleMs - 1);
  Refire r;
  EXPECT_FALSE(d.Due(kStart + kSettleMs, &r)) << "a change inside the period moved its start";
  ASSERT_TRUE(d.Due(kStart + kSettleMs - 1 + kSettleMs, &r));
  EXPECT_EQ(r.value, kB);
}

TEST(EquipmentRefire, ARemotePlayersRecordGivesNoRefire) {
  Debouncer d;
  EXPECT_EQ(d.Changed(Slot::kTacMod, kCs, kHandle, kA, /*local=*/false, /*server=*/false, /*changed=*/true, kStart), Verdict::kNotLocal);
  Refire r;
  EXPECT_FALSE(d.Pending(Slot::kTacMod));
  EXPECT_FALSE(d.Due(kStart + 10 * kSettleMs, &r));
}

TEST(EquipmentRefire, AChangeOnTheServerForARemotePlayerGivesNoRefire) {
  Debouncer d;
  EXPECT_EQ(d.Changed(Slot::kWeapon, kCs, kHandle, kA, /*local=*/false, /*server=*/true, /*changed=*/true, kStart), Verdict::kServer);
  Refire r;
  EXPECT_FALSE(d.Due(kStart + 10 * kSettleMs, &r));
}

TEST(EquipmentRefire, AServerNeverRefiresEvenWhenTheRecordLooksLocal) {
  Debouncer d;
  EXPECT_EQ(d.Changed(Slot::kOrdnance, kCs, kHandle, kA, /*local=*/true, /*server=*/true, /*changed=*/true, kStart), Verdict::kServer);
  Refire r;
  EXPECT_FALSE(d.Due(kStart + 10 * kSettleMs, &r));
}

TEST(EquipmentRefire, AValueTheRecordAlreadyHeldRunsNoHandlerAndIsNotTracked) {
  Debouncer d;
  EXPECT_EQ(d.Changed(Slot::kTacMod, kCs, kHandle, kA, true, false, /*changed=*/false, kStart), Verdict::kUnchanged);
  Refire r;
  EXPECT_FALSE(d.Due(kStart + 10 * kSettleMs, &r));
}

TEST(EquipmentRefire, AnUnchangedCallDoesNotMoveTheQuietPeriodOfAPendingChange) {
  Debouncer d;
  Local(d, Slot::kTacMod, kB, kStart);
  d.Changed(Slot::kTacMod, kCs, kHandle, kB, true, false, /*changed=*/false, kStart + kSettleMs - 1);
  Refire r;
  ASSERT_TRUE(d.Due(kStart + kSettleMs, &r));
  EXPECT_EQ(r.value, kB);
}

TEST(EquipmentRefire, TheThreeSlotsAreDebouncedApartAndComeOutInSlotOrder) {
  Debouncer d;
  Local(d, Slot::kOrdnance, kC, kStart);
  Local(d, Slot::kWeapon, kA, kStart + 100);
  Local(d, Slot::kTacMod, kB, kStart + 200);
  Refire r;
  ASSERT_TRUE(d.Due(kStart + 200 + kSettleMs, &r));
  EXPECT_EQ(r.slot, Slot::kWeapon);
  ASSERT_TRUE(d.Due(kStart + 200 + kSettleMs, &r));
  EXPECT_EQ(r.slot, Slot::kTacMod);
  ASSERT_TRUE(d.Due(kStart + 200 + kSettleMs, &r));
  EXPECT_EQ(r.slot, Slot::kOrdnance);
  EXPECT_FALSE(d.Due(kStart + 200 + kSettleMs, &r));
}

TEST(EquipmentRefire, AChangeAfterTheRefireStartsANewPeriod) {
  Debouncer d;
  Local(d, Slot::kTacMod, kA, kStart);
  Refire r;
  ASSERT_TRUE(d.Due(kStart + kSettleMs, &r));
  Local(d, Slot::kTacMod, kB, kStart + kSettleMs + 10);
  EXPECT_FALSE(d.Due(kStart + kSettleMs + 11, &r));
  ASSERT_TRUE(d.Due(kStart + 2 * kSettleMs + 10, &r));
  EXPECT_EQ(r.value, kB);
  EXPECT_EQ(r.changes, 1u);
}

TEST(EquipmentRefire, ADifferentRecordOfTheSameSlotReplacesTheEarlierOne) {
  Debouncer d;
  Local(d, Slot::kTacMod, kA, kStart);
  d.Changed(Slot::kTacMod, kCs + 0x100, kHandle + 1, kB, true, false, true, kStart + 10);
  Refire r;
  ASSERT_TRUE(d.Due(kStart + 10 + kSettleMs, &r));
  EXPECT_EQ(r.owner, kCs + 0x100);
  EXPECT_EQ(r.handle, kHandle + 1);
  EXPECT_EQ(r.changes, 1u);
}

TEST(EquipmentRefire, ADifferentHandleOnTheSameComponentIsADifferentRecord) {
  Debouncer d;
  Local(d, Slot::kTacMod, kA, kStart);
  d.Changed(Slot::kTacMod, kCs, kHandle + 1, kB, true, false, true, kStart + 10);
  Refire r;
  ASSERT_TRUE(d.Due(kStart + 10 + kSettleMs, &r));
  EXPECT_EQ(r.handle, kHandle + 1);
  EXPECT_EQ(r.changes, 1u) << "the burst of the first record is not the second's";
}

TEST(EquipmentRefire, AClockThatWentBackwardsWaitsAFullPeriodInsteadOfFiringEarly) {
  Debouncer d;
  Local(d, Slot::kTacMod, kA, kStart);
  Refire r;
  EXPECT_FALSE(d.Due(kStart - 5000, &r));
  EXPECT_FALSE(d.Due(kStart - 5000 + kSettleMs - 1, &r));
  EXPECT_TRUE(d.Due(kStart - 5000 + kSettleMs, &r));
}

TEST(EquipmentRefire, ClearDropsWhatWasPending) {
  Debouncer d;
  Local(d, Slot::kTacMod, kA, kStart);
  Local(d, Slot::kWeapon, kA, kStart);
  d.Clear();
  Refire r;
  EXPECT_FALSE(d.Pending(Slot::kTacMod));
  EXPECT_FALSE(d.Due(kStart + 10 * kSettleMs, &r));
}

// --- The component's layout, read through a reader (the same templates the Quest build compiles) ------------------------------

// A byte-addressed fake of the game's memory: only what was put is readable.
struct FakeMemory {
  std::map<std::uintptr_t, std::uint8_t> bytes;
  template <typename T>
  void Put(std::uintptr_t address, T value) {
    std::uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[address + i] = raw[i];
  }
  bool operator()(std::uintptr_t address, void* out, std::size_t size) const {
    for (std::size_t i = 0; i < size; ++i) {
      if (bytes.find(address + i) == bytes.end()) return false;
    }
    for (std::size_t i = 0; i < size; ++i) static_cast<std::uint8_t*>(out)[i] = bytes.at(address + i);
    return true;
  }
};

// One component with three records; sparse index 2 is dense index 1 with generation 3 (handle 0x00030002).
constexpr std::uintptr_t kTable = 0x5000, kRecords = 0x6000, kLinks = 0x7000, kCtx = 0x8000, kGame = 0x9000;
constexpr std::uint16_t kDense = 1, kGeneration = 3;

FakeMemory Component() {
  FakeMemory m;
  m.Put<std::uintptr_t>(kCs + nevr_equipment_refire::kIndexTableOffset, kTable);
  m.Put<std::uintptr_t>(kCs + nevr_equipment_refire::kRecordsOffset, kRecords);
  m.Put<std::uintptr_t>(kCs + nevr_equipment_refire::kLinkTableOffset, kLinks);
  m.Put<std::uintptr_t>(kCs + nevr_equipment_refire::kCtxOffset, kCtx);
  m.Put<std::uint16_t>(kCs + nevr_equipment_refire::kCountOffset, 3);
  m.Put<std::uint16_t>(kTable + 2 * 4, kDense);
  m.Put<std::uint16_t>(kTable + 2 * 4 + 2, kGeneration);
  const std::uintptr_t record = kRecords + kDense * nevr_equipment_refire::kRecordStride;
  m.Put<std::uint64_t>(record + 0x18, 0x1111);
  m.Put<std::uint64_t>(record + 0x20, 0x2222);
  m.Put<std::uint64_t>(record + 0x28, 0x3333);
  m.Put<std::uintptr_t>(kCtx + nevr_equipment_refire::kCtxNetGameArgOffset, 0xabc0);
  m.Put<std::uint64_t>(kCtx + nevr_equipment_refire::kCtxActorKeyOffset, 0xAC70);
  m.Put<std::uint64_t>(kLinks + kDense * nevr_equipment_refire::kLinkStride, 0x11CC);
  return m;
}

TEST(EquipmentRefireLayout, EachSlotIsReadFromItsOwnFieldOfTheRecord) {
  FakeMemory m = Component();
  std::uint64_t v = 0;
  ASSERT_TRUE(nevr_equipment_refire::RecordValue(m, kCs, kHandle, Slot::kWeapon, &v));
  EXPECT_EQ(v, 0x1111u);
  ASSERT_TRUE(nevr_equipment_refire::RecordValue(m, kCs, kHandle, Slot::kTacMod, &v));
  EXPECT_EQ(v, 0x2222u);
  ASSERT_TRUE(nevr_equipment_refire::RecordValue(m, kCs, kHandle, Slot::kOrdnance, &v));
  EXPECT_EQ(v, 0x3333u);
}

TEST(EquipmentRefireLayout, AHandleOfAnOlderGenerationNamesNoRecord) {
  FakeMemory m = Component();
  EXPECT_EQ(nevr_equipment_refire::DenseIndex(m, kCs, kHandle), 1);
  EXPECT_EQ(nevr_equipment_refire::DenseIndex(m, kCs, 0x00020002), -1);
  std::uint64_t v = 0;
  EXPECT_FALSE(nevr_equipment_refire::RecordValue(m, kCs, 0x00020002, Slot::kTacMod, &v));
}

TEST(EquipmentRefireLayout, ADenseIndexPastTheCountNamesNoRecord) {
  FakeMemory m = Component();
  m.Put<std::uint16_t>(kCs + nevr_equipment_refire::kCountOffset, 1);
  EXPECT_EQ(nevr_equipment_refire::DenseIndex(m, kCs, kHandle), -1);
}

TEST(EquipmentRefireLayout, AnUnreadableTableNamesNoRecord) {
  FakeMemory m = Component();
  m.bytes.erase(kTable + 2 * 4 + 2);
  EXPECT_EQ(nevr_equipment_refire::DenseIndex(m, kCs, kHandle), -1);
}

TEST(EquipmentRefireLayout, TheActorKeyIsTheContextKeyAndTheRecordsLink) {
  FakeMemory m = Component();
  ActorKey actor;
  ASSERT_TRUE(nevr_equipment_refire::ReadActorKey(m, kCs, kHandle, &actor));
  EXPECT_EQ(actor.netGameArg, 0xabc0u);
  EXPECT_EQ(actor.key[0], 0xAC70u);
  EXPECT_EQ(actor.key[1], 0x11CCu);
}

TEST(EquipmentRefireLayout, AnUnreadableLinkGivesNoActorKey) {
  FakeMemory m = Component();
  m.bytes.erase(kLinks + kDense * nevr_equipment_refire::kLinkStride);
  ActorKey actor;
  EXPECT_FALSE(nevr_equipment_refire::ReadActorKey(m, kCs, kHandle, &actor));
}

TEST(EquipmentRefireLayout, TheLocalSlotHandleIsSlotZerosGenerationAndMaskedField) {
  FakeMemory m;
  m.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotCountOffset, 2);
  m.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameLocalFieldOffset, 0x0025);  // masked to slot 5
  m.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotGenerationOffset + 5 * 4, 0x0007);
  std::uint32_t handle = 0;
  ASSERT_TRUE(nevr_equipment_refire::LocalSlotHandle(m, kGame, &handle));
  EXPECT_EQ(handle, 0x00070005u);
  EXPECT_EQ(nevr_equipment_refire::SlotHandle(0x0025, 7), 0x00070005u);
}

TEST(EquipmentRefireLayout, ANetGameWithNoSlotsInUseHasNoLocalSlot) {
  FakeMemory m;
  m.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotCountOffset, 0);
  m.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameLocalFieldOffset, 0x0001);
  m.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotGenerationOffset + 4, 1);
  std::uint32_t handle = 0;
  EXPECT_FALSE(nevr_equipment_refire::LocalSlotHandle(m, kGame, &handle));
  EXPECT_FALSE(nevr_equipment_refire::LocalSlotHandle(m, 0, &handle));
}

// The offsets are the game's own, as the setters read them (PC 0x140d43f00 and Quest 0x227cde4: +0xc8, +0xf8, 0x58, +0x20; PC
// 0x140d43d90 and Quest 0x227cde4's tail: ctx +0x58, +0x100 stride 0x10). The fake above is built from the same names, so the literals
// are pinned here.
TEST(EquipmentRefireLayout, TheOffsetsAreTheOnesTheSettersRead) {
  namespace n = nevr_equipment_refire;
  EXPECT_EQ(n::kCtxOffset, 0x80u);
  EXPECT_EQ(n::kIndexTableOffset, 0xc8u);
  EXPECT_EQ(n::kRecordsOffset, 0xf8u);
  EXPECT_EQ(n::kLinkTableOffset, 0x100u);
  EXPECT_EQ(n::kCountOffset, 0xf2u);
  EXPECT_EQ(n::kRecordStride, 0x58u);
  EXPECT_EQ(n::kLinkStride, 0x10u);
  EXPECT_EQ(n::kCtxNetGameArgOffset, 0x8u);
  EXPECT_EQ(n::kCtxActorKeyOffset, 0x58u);
  EXPECT_EQ(n::kRecordValueOffset[0], 0x18u);
  EXPECT_EQ(n::kRecordValueOffset[1], 0x20u);
  EXPECT_EQ(n::kRecordValueOffset[2], 0x28u);
  EXPECT_EQ(n::kGameSlotCountOffset, 0xdcu);
  EXPECT_EQ(n::kGameLocalFieldOffset, 0x178u);
  EXPECT_EQ(n::kGameSlotGenerationOffset, 0x13au);
  EXPECT_EQ(n::kSlotMask, 0x1fu);
  EXPECT_GE(n::kComponentSpan, n::kLinkTableOffset + 8) << "the span covers the fields the reads reach";
}

}  // namespace
