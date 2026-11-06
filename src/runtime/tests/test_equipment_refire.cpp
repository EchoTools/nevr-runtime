// When the equipment change the game dropped is applied again (patch/equipment_refire_rules.h, #2121). The hook that feeds the
// rule is patch/equipment_refire.cpp (PC) and quest/combat/equip_refire.cpp (Quest); the game's setters, handlers and the local-player
// check need the game and are proven by a live run. The Quest host test compiles this same header.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

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
  m.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotCount2Offset, 2);
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
  EXPECT_EQ(n::kGameSlotCount2Offset, 0xdeu);
  EXPECT_EQ(n::kGameLocalFieldOffset, 0x178u);
  EXPECT_EQ(n::kGameSlotGenerationOffset, 0x13au);
  EXPECT_EQ(n::kSlotMask, 0x1fu);
  EXPECT_GE(n::kComponentSpan, n::kLinkTableOffset + 8) << "the span covers the fields the reads reach";
}

// --- The feature above the hooks, driven through a fake Env (the same templates the PC and Quest glue instantiate) ------------------

constexpr std::uint16_t kVtableTag = 0x7777;

struct FakeEnv {
  static constexpr std::size_t kNetGameField = 0x8510;
  FakeMemory mem;
  std::uint64_t now = kStart;
  std::uint32_t slotOfResult = 0x00070001;  // the local player's handle: generation 7, slot 1
  std::vector<std::string> handlerCalls;
  std::vector<std::string> refired, skipped, changes;
  int netGameCalls = 0, locks = 0, unlocks = 0;
  bool operator()(std::uintptr_t a, void* out, std::size_t n) { return mem(a, out, n); }
  std::uintptr_t NetGame(std::uintptr_t arg) {
    ++netGameCalls;
    return arg == 0xabc0 ? kGame : 0;
  }
  std::uint32_t SlotOf(std::uintptr_t, const std::uint64_t key[2]) { return key[0] == 0xAC70 && key[1] == 0x11CC ? slotOfResult : 0xffffffffu; }
  void CallHandler(Slot slot, std::uintptr_t owner, std::uint32_t handle) {
    handlerCalls.push_back(std::string(nevr_equipment_refire::SlotName(slot)) + ":" + std::to_string(owner) + ":" + std::to_string(handle));
  }
  std::uint64_t NowMs() { return now; }
  void Lock() { ++locks; }
  void Unlock() { ++unlocks; }
  void LogChange(Slot, const nevr_equipment_refire::SetterResult& r, std::uint32_t, std::uint64_t) {
    changes.push_back(nevr_equipment_refire::VerdictName(r.verdict));
  }
  void LogRefired(const Refire& r) { refired.push_back(std::string(nevr_equipment_refire::SlotName(r.slot)) + ":" + std::to_string(r.changes)); }
  void LogSkipped(const Refire& r, const char* why) { skipped.push_back(std::string(nevr_equipment_refire::SlotName(r.slot)) + ":" + why); }
};

// A component with three records (Component()), every byte of its span readable, a vtable word, and a net game whose slot 0 is the
// local player (handle 0x00070001).
FakeEnv World() {
  FakeEnv e;
  e.mem = Component();
  for (std::size_t i = 0; i < nevr_equipment_refire::kComponentSpan; ++i) {
    if (e.mem.bytes.find(kCs + i) == e.mem.bytes.end()) e.mem.bytes[kCs + i] = 0;
  }
  e.mem.Put<std::uintptr_t>(kCs, 0xBEEF0000u + kVtableTag);
  e.mem.Put<std::uintptr_t>(0xabc0 + FakeEnv::kNetGameField, kGame);
  e.mem.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotCountOffset, 2);
  e.mem.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotCount2Offset, 2);
  e.mem.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameLocalFieldOffset, 0x0001);
  e.mem.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotGenerationOffset + 1 * 4, 7);
  return e;
}

constexpr std::uintptr_t kTacRecord = kRecords + kDense * nevr_equipment_refire::kRecordStride + 0x20;

// The game's setter, as the hook sees it: it writes the value into the record.
auto Game(FakeEnv& e, std::uintptr_t field, std::uint64_t value, int* calls = nullptr) {
  return [&e, field, value, calls] {
    if (calls != nullptr) ++*calls;
    e.mem.Put<std::uint64_t>(field, value);
  };
}

TEST(EquipmentRefireFeature, ALocalChangeIsTrackedAndTheOriginalRunsOnceBeforeItIsJudged) {
  FakeEnv e = World();
  nevr_equipment_refire::Tracker t;
  int calls = 0;
  const auto r = nevr_equipment_refire::OnSetter(e, t, Slot::kTacMod, kCs, kHandle, 0x9999, false, Game(e, kTacRecord, 0x9999, &calls));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(r.verdict, Verdict::kTracked);
  EXPECT_TRUE(r.read);
  EXPECT_TRUE(r.changed) << "the value was read before the game's setter overwrote it";
  EXPECT_TRUE(r.local);
  EXPECT_TRUE(t.debouncer.Pending(Slot::kTacMod));
  EXPECT_EQ(t.vtable[1], 0xBEEF0000u + kVtableTag);
  EXPECT_EQ(e.locks, e.unlocks);
  ASSERT_EQ(e.changes.size(), 1u);
  EXPECT_EQ(e.changes[0], "tracked");
}

TEST(EquipmentRefireFeature, AnotherPlayersRecordIsNotTracked) {
  FakeEnv e = World();
  e.slotOfResult = 0x00070002;
  nevr_equipment_refire::Tracker t;
  const auto r = nevr_equipment_refire::OnSetter(e, t, Slot::kTacMod, kCs, kHandle, 0x9999, false, Game(e, kTacRecord, 0x9999));
  EXPECT_EQ(r.verdict, Verdict::kNotLocal);
  EXPECT_TRUE(r.changed);
  EXPECT_FALSE(r.local);
  EXPECT_FALSE(t.debouncer.Pending(Slot::kTacMod));
}

TEST(EquipmentRefireFeature, ACallThatChangedNothingIsUnchangedAndNobodyIsAskedWhoOwnsTheRecord) {
  FakeEnv e = World();
  nevr_equipment_refire::Tracker t;
  int calls = 0;
  const auto r = nevr_equipment_refire::OnSetter(e, t, Slot::kTacMod, kCs, kHandle, 0x2222, false, Game(e, kTacRecord, 0x2222, &calls));
  EXPECT_EQ(calls, 1) << "the game's setter still runs";
  EXPECT_EQ(r.verdict, Verdict::kUnchanged);
  EXPECT_FALSE(r.changed);
  EXPECT_EQ(e.netGameCalls, 0);
  EXPECT_FALSE(t.debouncer.Pending(Slot::kTacMod));
}

TEST(EquipmentRefireFeature, ARecordThatCannotBeReadIsUnreadableNotUnchanged) {
  FakeEnv e = World();
  nevr_equipment_refire::Tracker t;
  int calls = 0;
  const auto r = nevr_equipment_refire::OnSetter(e, t, Slot::kTacMod, kCs, 0x00020002, 0x9999, false, Game(e, kTacRecord, 0x9999, &calls));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(r.verdict, Verdict::kUnreadable);
  EXPECT_FALSE(r.read);
  EXPECT_FALSE(t.debouncer.Pending(Slot::kTacMod));
}

TEST(EquipmentRefireFeature, AServerReadsNothingTracksNothingAndStillRunsTheOriginal) {
  FakeEnv e = World();
  nevr_equipment_refire::Tracker t;
  int calls = 0;
  const auto r = nevr_equipment_refire::OnSetter(e, t, Slot::kTacMod, kCs, kHandle, 0x9999, true, Game(e, kTacRecord, 0x9999, &calls));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(r.verdict, Verdict::kServer);
  EXPECT_FALSE(r.read);
  EXPECT_EQ(e.netGameCalls, 0);
  EXPECT_FALSE(t.debouncer.Pending(Slot::kTacMod));
}

TEST(EquipmentRefireFeature, TheLocalCheckFailsClosedOnEveryUnreadableStep) {
  {
    FakeEnv e = World();
    EXPECT_TRUE(nevr_equipment_refire::IsLocalRecord(e, kCs, kHandle));
  }
  {
    FakeEnv e = World();
    e.slotOfResult = 0x00070002;
    EXPECT_FALSE(nevr_equipment_refire::IsLocalRecord(e, kCs, kHandle)) << "the record belongs to another slot";
  }
  {
    FakeEnv e = World();
    e.mem.bytes.erase(0xabc0 + FakeEnv::kNetGameField);
    EXPECT_FALSE(nevr_equipment_refire::IsLocalRecord(e, kCs, kHandle)) << "the getter's field is probed before the game reads it";
    EXPECT_EQ(e.netGameCalls, 0);
  }
  {
    FakeEnv e = World();
    e.mem.Put<std::uint64_t>(kCtx + nevr_equipment_refire::kCtxNetGameArgOffset, 0xdead);
    e.mem.Put<std::uint64_t>(0xdead + FakeEnv::kNetGameField, 0);
    EXPECT_FALSE(nevr_equipment_refire::IsLocalRecord(e, kCs, kHandle)) << "no net game";
  }
  // A count of zero or an unreadable local slot is "not local" even when the lookup would answer the very handle (or 0).
  {
    FakeEnv e = World();
    e.mem.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotCount2Offset, 0);
    EXPECT_FALSE(nevr_equipment_refire::IsLocalRecord(e, kCs, kHandle)) << "SetTier needs both counts: +0xde";
  }
  {
    FakeEnv e = World();
    e.mem.Put<std::uint16_t>(kGame + nevr_equipment_refire::kGameSlotCountOffset, 0);
    EXPECT_FALSE(nevr_equipment_refire::IsLocalRecord(e, kCs, kHandle)) << "SetTier needs both counts: +0xdc";
  }
  {
    FakeEnv e = World();
    e.slotOfResult = 0;
    e.mem.bytes.erase(kGame + nevr_equipment_refire::kGameLocalFieldOffset);
    EXPECT_FALSE(nevr_equipment_refire::IsLocalRecord(e, kCs, kHandle)) << "the local slot's field is unreadable";
  }
  {
    FakeEnv e = World();
    e.mem.bytes.erase(kLinks + kDense * nevr_equipment_refire::kLinkStride);
    EXPECT_FALSE(nevr_equipment_refire::IsLocalRecord(e, kCs, kHandle));
  }
}

// Each reason a due re-fire is refused, and the one case where it is made.
TEST(EquipmentRefireFeature, AStaleRefireIsNamedByTheFirstCheckItFails) {
  FakeEnv e = World();
  nevr_equipment_refire::Tracker t;
  nevr_equipment_refire::OnSetter(e, t, Slot::kTacMod, kCs, kHandle, 0x9999, false, Game(e, kTacRecord, 0x9999));
  Refire due;
  e.now = kStart + kSettleMs;
  ASSERT_TRUE(t.debouncer.Due(e.now, &due));
  EXPECT_EQ(nevr_equipment_refire::StaleReason(e, t, due), nullptr);
  {
    FakeEnv f = e;
    f.mem.bytes.erase(kCs + 0x100);
    EXPECT_STREQ(nevr_equipment_refire::StaleReason(f, t, due), "component_unreadable");
  }
  {
    FakeEnv f = e;
    f.mem.Put<std::uintptr_t>(kCs, 0x12345678);
    EXPECT_STREQ(nevr_equipment_refire::StaleReason(f, t, due), "component_replaced");
  }
  {
    nevr_equipment_refire::Tracker none;
    EXPECT_STREQ(nevr_equipment_refire::StaleReason(e, none, due), "component_replaced") << "nothing was tracked for this slot";
  }
  {
    FakeEnv f = e;
    f.mem.Put<std::uint16_t>(kTable + 2 * 4 + 2, 4);
    EXPECT_STREQ(nevr_equipment_refire::StaleReason(f, t, due), "record_gone");
  }
  {
    FakeEnv f = e;
    f.mem.Put<std::uint64_t>(kTacRecord, 0x5555);
    EXPECT_STREQ(nevr_equipment_refire::StaleReason(f, t, due), "record_changed_elsewhere");
  }
  {
    FakeEnv f = e;
    f.slotOfResult = 0x00070002;
    EXPECT_STREQ(nevr_equipment_refire::StaleReason(f, t, due), "not_local");
  }
}

TEST(EquipmentRefireFeature, FireDueRunsTheSlotsHandlerOnceAfterTheSettleTimeAndLogsIt) {
  FakeEnv e = World();
  nevr_equipment_refire::Tracker t;
  nevr_equipment_refire::OnSetter(e, t, Slot::kTacMod, kCs, kHandle, 0x9999, false, Game(e, kTacRecord, 0x9999));
  e.now = kStart + kSettleMs - 1;
  EXPECT_EQ(nevr_equipment_refire::FireDue(e, t), 0u);
  EXPECT_TRUE(e.handlerCalls.empty());
  e.now = kStart + kSettleMs;
  EXPECT_EQ(nevr_equipment_refire::FireDue(e, t), 1u);
  ASSERT_EQ(e.handlerCalls.size(), 1u);
  EXPECT_EQ(e.handlerCalls[0], "tac_mod:" + std::to_string(kCs) + ":" + std::to_string(kHandle));
  ASSERT_EQ(e.refired.size(), 1u);
  EXPECT_EQ(e.refired[0], "tac_mod:1");
  EXPECT_TRUE(e.skipped.empty());
  EXPECT_EQ(nevr_equipment_refire::FireDue(e, t), 0u) << "once per quiet period";
  EXPECT_EQ(e.locks, e.unlocks);
}

TEST(EquipmentRefireFeature, FireDueSkipsAStaleRefireWithItsReasonAndCallsNoHandler) {
  FakeEnv e = World();
  nevr_equipment_refire::Tracker t;
  nevr_equipment_refire::OnSetter(e, t, Slot::kTacMod, kCs, kHandle, 0x9999, false, Game(e, kTacRecord, 0x9999));
  e.mem.Put<std::uint64_t>(kTacRecord, 0x5555);  // something else changed the choice meanwhile
  e.now = kStart + kSettleMs;
  EXPECT_EQ(nevr_equipment_refire::FireDue(e, t), 0u);
  EXPECT_TRUE(e.handlerCalls.empty());
  EXPECT_TRUE(e.refired.empty());
  ASSERT_EQ(e.skipped.size(), 1u);
  EXPECT_EQ(e.skipped[0], "tac_mod:record_changed_elsewhere");
}

TEST(EquipmentRefireFeature, FireDueHandlesEveryDueSlotInSlotOrderWithItsOwnHandler) {
  FakeEnv e = World();
  nevr_equipment_refire::Tracker t;
  const std::uintptr_t record = kRecords + kDense * nevr_equipment_refire::kRecordStride;
  nevr_equipment_refire::OnSetter(e, t, Slot::kOrdnance, kCs, kHandle, 0x31, false, Game(e, record + 0x28, 0x31));
  nevr_equipment_refire::OnSetter(e, t, Slot::kWeapon, kCs, kHandle, 0x11, false, Game(e, record + 0x18, 0x11));
  e.now = kStart + kSettleMs;
  EXPECT_EQ(nevr_equipment_refire::FireDue(e, t), 2u);
  ASSERT_EQ(e.handlerCalls.size(), 2u);
  EXPECT_EQ(e.handlerCalls[0].substr(0, 7), "weapon:");
  EXPECT_EQ(e.handlerCalls[1].substr(0, 9), "ordnance:");
}

TEST(EquipmentRefireFeature, TheLogWordsNameTheConsequence) {
  EXPECT_NE(std::string(nevr_equipment_refire::kMsgRefired).find("change handler ran once more"), std::string::npos);
  EXPECT_NE(std::string(nevr_equipment_refire::kMsgSkipped).find("left as the game made it"), std::string::npos);
  EXPECT_STREQ(nevr_equipment_refire::kMsgChange, "equipment change");
  EXPECT_STREQ(nevr_equipment_refire::VerdictName(Verdict::kUnreadable), "unreadable");
}

}  // namespace
