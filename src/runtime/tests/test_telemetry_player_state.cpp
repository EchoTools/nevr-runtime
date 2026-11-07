/* SYNTHESIS -- custom tool code, not from binary */

// A player's stunned, invulnerable, blocking and emote state in the telemetry stream (#2269), read the way
// echovr.exe 34.4.631547.1's /session writers read them. The offsets and bits below are written out from the
// disassembly, not taken from the shipped headers, so a wrong constant there fails here:
//   - the entity: the 16 bytes at net game + 0x178 + i * 0x250 + 0x08 go to the resolver *(net game + 0x08),
//     their first 8 then to the entity lookup on the same object (game-state writer 0x14015727a-0x1401572b2).
//     No entity, nothing is read;
//   - stunned, invulnerable, blocking: GetComponent<R14NetPunchable>(out, entity, handle[1], -1)
//     (0x140167fc0), the component's check, vfunc +0x1C8, with the index (0x140157393-0x1401573b0); the row
//     *(component + 0xF8) + table[index & 0xFFFF] * 0x48, table *(component + 0xC8) with a u16 every 4 bytes;
//     the row's dword at +0x04: stunned bit 1, invulnerable bit 2, blocking any of bits 5-7
//     (0x1401573b6-0x140157401);
//   - is_emote_playing: GetComponent<R15NetCustomization> (0x1401679a0), the same check, then what
//     0x140d08d00(component, index) returns (player writer 0x1401b3fb7-0x1401b3fdf).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "runtime/server/telemetry_player_state.h"

namespace {

using nevr_telemetry_pose::ComponentRef;
using nevr_telemetry_state::StateFunctions;

constexpr uint64_t kEntitySystem = 0x08;
constexpr uint64_t kEntrants = 0x178;
constexpr uint64_t kEntrantStride = 0x250;
constexpr uint64_t kEntrantHandle = 0x08;

constexpr uint64_t kCheckSlot = 0x1C8 / 8;
constexpr uint64_t kPunchableTable = 0xC8;
constexpr uint64_t kPunchableRows = 0xF8;
constexpr uint64_t kPunchableStride = 0x48;
constexpr uint64_t kPunchableFlags = 0x04;

// Whether a fake component's check accepts it: bytes no real offset above uses.
constexpr uint64_t kFakeAccept = 0x1F0;

using CheckFn = int(__fastcall*)(void*, uint32_t);

template <typename T>
void PutAt(uint8_t* base, uint64_t off, T value) {
  std::memcpy(base + off, &value, sizeof(T));
}

struct GetCall {
  void* entity;
  int64_t extra;
  int64_t any;
};
struct Call2 {
  void* object;
  uint32_t index;
};

struct Recorder {
  int resolveCalls = 0;
  void* resolver = nullptr;
  int64_t resolveIn[2] = {};
  std::vector<std::pair<void*, int64_t>> lookups;
  std::map<int64_t, void*> entities;
  std::vector<GetCall> punchableGets, customizationGets;
  std::vector<Call2> emoteReads;
  int emoteResult = 0;
};
Recorder g;

class Component {
 public:
  Component() {
    std::memset(raw_, 0, sizeof(raw_));
    const CheckFn* vtable = Vtable();
    std::memcpy(raw_, &vtable, sizeof(vtable));
    Accept(true);
  }
  void* Ptr() { return raw_; }
  void Accept(bool accept) { PutAt<uint32_t>(raw_, kFakeAccept, accept ? 1 : 0); }
  template <typename T>
  void Put(uint64_t off, T value) {
    PutAt<T>(raw_, off, value);
  }

  static int __fastcall Check(void* component, uint32_t) {
    uint32_t accept;
    std::memcpy(&accept, static_cast<uint8_t*>(component) + kFakeAccept, sizeof(accept));
    return static_cast<int>(accept);
  }

 private:
  static const CheckFn* Vtable() {
    static CheckFn vtable[64] = {};
    vtable[kCheckSlot] = &Check;
    return vtable;
  }
  alignas(16) uint8_t raw_[0x200];
};

// An R14NetPunchable component whose index `index` names row `row`, whose flags are `flags`; the other rows
// carry every bit, so a wrong row or stride reads as all three set.
class Punchable {
 public:
  Punchable() : table_(64, 0), rows_(8 * kPunchableStride, 0) {
    for (uint64_t r = 0; r < 8; r++) PutAt<uint32_t>(rows_.data(), r * kPunchableStride + kPunchableFlags, 0xFFFFFFFFu);
    component_.Put<const uint8_t*>(kPunchableTable, table_.data());
    component_.Put<const uint8_t*>(kPunchableRows, rows_.data());
  }
  void Set(uint32_t index, uint16_t row, uint32_t flags) {
    PutAt<uint16_t>(table_.data(), (index & 0xFFFF) * 4, row);
    PutAt<uint32_t>(rows_.data(), row * kPunchableStride + kPunchableFlags, flags);
  }
  Component component_;
  std::vector<uint8_t> table_;
  std::vector<uint8_t> rows_;
};

// What GetComponent<T> finds on a fake entity.
struct Entity {
  Component* punchable = nullptr;
  uint32_t punchableIndex = 0;
  Component* customization = nullptr;
  uint32_t customizationIndex = 0;
};

int __fastcall FakeResolve(void* resolver, int64_t* handle) {
  g.resolveCalls++;
  g.resolver = resolver;
  g.resolveIn[0] = handle[0];
  g.resolveIn[1] = handle[1];
  handle[0] += 0x1000;  // the resolver rewrites the id; the lookup must get the rewritten one
  handle[1] += 0x20;    // and GetComponent the second half as it is after the call
  return 1;
}

void* __fastcall FakeLookup(void* system, int64_t id) {
  g.lookups.push_back({system, id});
  const auto found = g.entities.find(id);
  return found == g.entities.end() ? nullptr : found->second;
}

void Fill(ComponentRef* out, Component* component, uint32_t index) {
  out->component = component ? component->Ptr() : nullptr;
  out->index = index;
}
void __fastcall FakePunchableComponent(ComponentRef* out, void* entity, int64_t extra, int64_t any) {
  g.punchableGets.push_back({entity, extra, any});
  const auto* e = static_cast<Entity*>(entity);
  Fill(out, e->punchable, e->punchableIndex);
}
void __fastcall FakeCustomizationComponent(ComponentRef* out, void* entity, int64_t extra, int64_t any) {
  g.customizationGets.push_back({entity, extra, any});
  const auto* e = static_cast<Entity*>(entity);
  Fill(out, e->customization, e->customizationIndex);
}
int __fastcall FakeEmotePlaying(void* component, uint32_t index) {
  g.emoteReads.push_back({component, index});
  return g.emoteResult;
}

StateFunctions Fakes() {
  StateFunctions fn;
  fn.resolveHandle = &FakeResolve;
  fn.lookupEntity = &FakeLookup;
  fn.punchableComponent = &FakePunchableComponent;
  fn.customizationComponent = &FakeCustomizationComponent;
  fn.emotePlaying = &FakeEmotePlaying;
  return fn;
}

class TelemetryPlayerState : public ::testing::Test {
 protected:
  void SetUp() override {
    g = Recorder{};
    netGame_.assign(0x3000, 0);
    PutAt<void*>(netGame_.data(), kEntitySystem, &entitySystem_);
    entity_.punchable = &punchable_.component_;
    entity_.punchableIndex = 0x00070003;  // the row table is indexed by the low 16 bits
    entity_.customization = &customization_;
    entity_.customizationIndex = 11;
    punchable_.Set(entity_.punchableIndex, 2, 0);
  }

  void PlaceEntity(uint16_t entrant, int64_t id, Entity* entity) {
    const uint64_t at = kEntrants + entrant * kEntrantStride + kEntrantHandle;
    PutAt<int64_t>(netGame_.data(), at, id);
    PutAt<int64_t>(netGame_.data(), at + 8, 0x55);
    g.entities[id + 0x1000] = entity;
  }

  TelemetrySnapshot::PlayerInfo Read(uint16_t entrant, const StateFunctions& fn = Fakes()) {
    TelemetrySnapshot::PlayerInfo info{};
    nevr_telemetry_state::ReadPlayerState(netGame_.data(), entrant, fn, &info);
    return info;
  }

  std::vector<uint8_t> netGame_;
  uint8_t entitySystem_[16] = {};
  Punchable punchable_;
  Component customization_;
  Entity entity_;
};

TEST_F(TelemetryPlayerState, EachFlagIsTheBitTheGameStateWriterReads) {
  PlaceEntity(3, 0x40, &entity_);
  struct Case {
    uint32_t flags;
    bool stunned, invulnerable, blocking;
  };
  const Case cases[] = {
      {0x00, false, false, false}, {0x02, true, false, false}, {0x04, false, true, false},
      {0x20, false, false, true},  {0x40, false, false, true}, {0x80, false, false, true},
      {0x01, false, false, false}, {0x08, false, false, false}, {0x10, false, false, false},
      {0x100, false, false, false}, {0xE6, true, true, true},
  };
  for (const Case& c : cases) {
    punchable_.Set(entity_.punchableIndex, 2, c.flags);
    const auto info = Read(3);
    EXPECT_EQ(info.stunned, c.stunned) << "flags 0x" << std::hex << c.flags;
    EXPECT_EQ(info.invulnerable, c.invulnerable) << "flags 0x" << std::hex << c.flags;
    EXPECT_EQ(info.blocking, c.blocking) << "flags 0x" << std::hex << c.flags;
  }
}

TEST_F(TelemetryPlayerState, TheRowIsTheTableEntryOfTheIndexsLowWord) {
  PlaceEntity(3, 0x40, &entity_);
  punchable_.Set(entity_.punchableIndex, 5, 0x02);  // row 5: stunned only; every other row has every bit
  const auto info = Read(3);
  EXPECT_TRUE(info.stunned);
  EXPECT_FALSE(info.invulnerable);
  EXPECT_FALSE(info.blocking);
}

TEST_F(TelemetryPlayerState, TheEntityIsTheEntrantsAndEachComponentIsAskedWithItsHandle) {
  PlaceEntity(6, 0x40, &entity_);
  Read(6);
  EXPECT_EQ(g.resolveCalls, 1);
  EXPECT_EQ(g.resolver, &entitySystem_);
  EXPECT_EQ(g.resolveIn[0], 0x40);
  EXPECT_EQ(g.resolveIn[1], 0x55);
  ASSERT_EQ(g.lookups.size(), 1u);
  EXPECT_EQ(g.lookups[0].first, &entitySystem_);
  EXPECT_EQ(g.lookups[0].second, 0x1040);
  ASSERT_EQ(g.punchableGets.size(), 1u);
  EXPECT_EQ(g.punchableGets[0].entity, &entity_);
  EXPECT_EQ(g.punchableGets[0].extra, 0x75);
  EXPECT_EQ(g.punchableGets[0].any, -1);
  ASSERT_EQ(g.customizationGets.size(), 1u);
  EXPECT_EQ(g.customizationGets[0].entity, &entity_);
  EXPECT_EQ(g.customizationGets[0].extra, 0x75);
  EXPECT_EQ(g.customizationGets[0].any, -1);
}

TEST_F(TelemetryPlayerState, TheEmoteIsWhatTheGamesReaderReturns) {
  PlaceEntity(3, 0x40, &entity_);
  g.emoteResult = 1;
  EXPECT_TRUE(Read(3).emotePlaying);
  ASSERT_EQ(g.emoteReads.size(), 1u);
  EXPECT_EQ(g.emoteReads[0].object, customization_.Ptr());
  EXPECT_EQ(g.emoteReads[0].index, 11u);
  g.emoteResult = 0;
  EXPECT_FALSE(Read(3).emotePlaying);
}

TEST_F(TelemetryPlayerState, NoEntityReadsNothing) {
  PlaceEntity(3, 0x40, &entity_);
  punchable_.Set(entity_.punchableIndex, 2, 0xE6);
  g.emoteResult = 1;
  const auto info = Read(4);  // entrant 4 has no entity
  EXPECT_FALSE(info.stunned || info.invulnerable || info.blocking || info.emotePlaying);
  EXPECT_TRUE(g.punchableGets.empty());
  EXPECT_TRUE(g.customizationGets.empty());
  EXPECT_TRUE(g.emoteReads.empty());
}

TEST_F(TelemetryPlayerState, AComponentItsCheckRefusesIsNotRead) {
  PlaceEntity(3, 0x40, &entity_);
  punchable_.Set(entity_.punchableIndex, 2, 0xE6);
  g.emoteResult = 1;
  punchable_.component_.Accept(false);
  customization_.Accept(false);
  const auto info = Read(3);
  EXPECT_FALSE(info.stunned || info.invulnerable || info.blocking);
  EXPECT_FALSE(info.emotePlaying);
  EXPECT_TRUE(g.emoteReads.empty());
}

TEST_F(TelemetryPlayerState, NoComponentOrNoTablesReadsNothing) {
  PlaceEntity(3, 0x40, &entity_);
  punchable_.Set(entity_.punchableIndex, 2, 0xE6);
  entity_.customization = nullptr;
  g.emoteResult = 1;
  punchable_.component_.Put<const uint8_t*>(kPunchableRows, nullptr);
  auto info = Read(3);
  EXPECT_FALSE(info.stunned || info.invulnerable || info.blocking || info.emotePlaying);
  EXPECT_TRUE(g.emoteReads.empty());

  entity_.punchable = nullptr;
  info = Read(3);
  EXPECT_FALSE(info.stunned || info.invulnerable || info.blocking);
}

TEST_F(TelemetryPlayerState, WithoutEveryFunctionNothingIsCalled) {
  PlaceEntity(3, 0x40, &entity_);
  punchable_.Set(entity_.punchableIndex, 2, 0xE6);
  StateFunctions fn = Fakes();
  fn.emotePlaying = nullptr;
  const auto info = Read(3, fn);
  EXPECT_FALSE(info.stunned);
  EXPECT_EQ(g.resolveCalls, 0);
}

// A slot-9 player (entrant word 0b01001 in its slot bits) whose entity is neither stunned nor emoting reads as
// neither: the state comes from its entity, at the slot ReadPlayers put it in.
TEST_F(TelemetryPlayerState, EachPresentSlotIsReadFromItsEntrant) {
  Punchable stunnedPunchable;
  Entity stunnedEntity;
  stunnedEntity.punchable = &stunnedPunchable.component_;
  stunnedEntity.punchableIndex = 1;
  stunnedPunchable.Set(1, 0, 0x02);
  PlaceEntity(4, 0x40, &entity_);        // entrant 4 is slot 9: nothing set
  PlaceEntity(1, 0x80, &stunnedEntity);  // entrant 1 is slot 2: stunned

  TelemetrySnapshot snap;
  snap.Clear();
  snap.playerInfo[9].present = true;
  snap.playerInfo[9].entrantIndex = 4;
  snap.playerInfo[9].entrantWord = 9;
  snap.playerInfo[2].present = true;
  snap.playerInfo[2].entrantIndex = 1;
  snap.playerInfo[5].entrantIndex = 4;  // not present: not read
  const auto result = nevr_telemetry_state::ReadPlayerStates(netGame_.data(), &snap, Fakes());
  EXPECT_EQ(result.withEntity, 2u);
  EXPECT_EQ(result.noEntity, 0u);
  EXPECT_FALSE(snap.playerInfo[9].stunned);
  EXPECT_FALSE(snap.playerInfo[9].emotePlaying);
  EXPECT_TRUE(snap.playerInfo[2].stunned);
  EXPECT_FALSE(snap.playerInfo[5].stunned || snap.playerInfo[5].blocking);
  EXPECT_EQ(g.resolveCalls, 2);
}

}  // namespace
