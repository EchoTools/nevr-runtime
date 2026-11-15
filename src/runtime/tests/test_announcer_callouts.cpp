// The announcer callouts (runtime/combat/announcer_callouts.h, #2242): the lines the handler matches, the thread start it
// makes for PULSAR, the event list the query extends, the override the read substitutes, the records gate, the free
// listener, and the wiring: the three rows installed through the script hook table on a fake announcer image, each
// detour bound to its entry. The stock script is not loaded. The handler gets a fake script (its counters pointer at
// kScriptCountersAt) and a fake event; the start is a fake function that records its call; the read gets a fake output
// state with the event at kOutputEventAt.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/announcer_callouts.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace announcer = nevr_combat::announcer;
namespace sh = nevr_script_hooks;

namespace {

// The stock immediate of the handler's first check (0x4B8B: movabs rax, evt_vo_hra_equip_ability_sensor).
constexpr std::uint64_t kAbilitySensor = 0xFAB7910180661E12ULL;
// The stock immediate of the chain listener's check (0x4DD7: cmp [rbx], rax; the event is blazar's line).
constexpr std::uint64_t kStockChainEvent = 0xB7E12DEAAB417432ULL;
constexpr std::uint32_t kChainListenerNode = 0x9B7C22FBu;  // the stock chain listener's node
constexpr std::uint32_t kStockChainWwise = 0x5E486409u;    // the Wwise event the stock chain node plays
// The Wwise id of PULSAR's line in fix_vo.py's FNV-1, and the lower-case test of it.
constexpr const char* kPulsarWwiseName = "vo_hra_gun_equip_pulsar";
constexpr const char* kPulsarWwiseNameUpper = "VO_HRA_GUN_EQUIP_PULSAR";
constexpr const char* kTranslocatorGameName = "evt_vo_hra_equip_grenade_loc";
constexpr std::size_t kBlazarIndex = announcer::kLineCount - 1;  // blazar is the last line (fix_vo.py's order)
constexpr std::size_t kLinesWithoutAListener = announcer::kAddedNames;  // the first six lines have no stock listener
constexpr std::size_t kFirstFakeLine = 0;

// The fake thread start's node entry: its +8 holds the data slot's address, the rest zero.
constexpr std::size_t kEntrySize = 0x80;
constexpr std::size_t kEntryDataSlotAt = 0x08;
constexpr std::size_t kEntryWordAt = 0x50;
constexpr std::size_t kEntryWord2At = 0x58;
constexpr std::size_t kEntryCounterAddrAt = 0x70;
constexpr std::size_t kScriptCountersAt = 0x68;
constexpr std::size_t kScriptSize = 0x80;
constexpr std::size_t kCounterCount = 8;
constexpr std::size_t kCounterIndex = announcer::kPulsarCounter / sizeof(std::int32_t);
constexpr std::size_t kOutputSize = 0x40;
constexpr std::size_t kOutputEventAt = 0x28;
constexpr std::size_t kTestEventWord = 0x1800;   // event+0x18
constexpr std::size_t kTestEventWord2 = 0x2800;  // event+0x28
constexpr std::size_t kTestEventData = 0x3000;   // event+0x30
constexpr std::size_t kEventWordCount = 8;
constexpr std::size_t kEventThreadWordIndex = 3;
constexpr std::size_t kEventThreadWord2Index = 5;
constexpr std::size_t kEventDataIndex = 6;
constexpr std::uint32_t kOtherNodeEvent = 0xB25704D7u;  // one of the nineteen other node events (the node at 0x3b60)
constexpr std::uint32_t kSentinelOverride = 0x4242u;
constexpr std::uint32_t kUntouchedOverride = 0x1234u;
constexpr std::uint64_t kOtherKeyDelta = 2;
constexpr std::uint64_t kStockReadResult = 0;
constexpr std::uint32_t kStockQueryAnswer = 1;
constexpr std::size_t kGameTableEntryFill = 0;
constexpr std::size_t kTestFakeImageSize = 0x6000;
constexpr std::uint32_t kImageHandlerRva = announcer::kHandlerRva;
constexpr std::uint32_t kImageQueryRva = announcer::kEventsQueryRva;
constexpr std::uint32_t kImageReadRva = announcer::kReadContextRva;
constexpr std::size_t kJumpRelativeLength = 5;         // E9 rel32
constexpr std::uint8_t kJumpRelativeOpcode = 0xE9;
constexpr std::uint8_t kJumpIndirectOpcode0 = 0xFF;
constexpr std::uint8_t kJumpIndirectOpcode1 = 0x25;
constexpr std::size_t kJumpAbsoluteTargetAt = 6;
constexpr std::size_t kCorruptPinIndex = 0;
constexpr std::uint8_t kCorruptPinMask = 0xFF;  // flips every bit of the pinned byte
constexpr std::uint8_t kFakeImageFill = 0xCC;   // int3: bytes no row's pin matches
constexpr std::uint32_t kPostersLabelIndex = 0;  // labels[0] is lobby_posters, labels[1] is combat_content
constexpr std::uint32_t kCombatLabelIndex = 1;
constexpr std::uint64_t kOtherRecordType = 0x1111;
constexpr std::uint64_t kOtherRecordName = 0x2222;
constexpr std::uint32_t kTestRecordSize = 16;
constexpr std::uint32_t kTestChunkBytes = 64;
constexpr std::int64_t kTestPackageHandle = 0;
constexpr int kMaxBackJumpScan = 64;     // bytes of a trampoline searched for its jump back
constexpr int kMaxCopiedPrologue = 32;   // the most a trampoline copies from an entry before it jumps back
constexpr bool kBootSet = true;
constexpr bool kFetchedSet = false;

// The stock bytes at the three entries, copied from the stock file (see announcer_callouts.h): the test's own copy, so a
// wrong pin in the header is not compared with itself.
constexpr std::uint8_t kStockHandlerBytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0xb8,
                                               0x95, 0x83, 0xe7, 0xf5};
constexpr std::uint8_t kStockQueryBytes[] = {0x48, 0xb8, 0x0d, 0x13, 0x2d, 0x1f, 0x2b, 0xc2, 0xd2, 0x41, 0x48, 0x3b,
                                             0xc8, 0x75, 0x17, 0x48, 0x8d, 0x05, 0xba, 0x6d, 0x00, 0x00, 0x48, 0x89,
                                             0x02, 0x41, 0xc7, 0x00, 0x14, 0x00, 0x00, 0x00};
constexpr std::uint8_t kStockReadBytes[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83,
                                            0x7a, 0x18, 0x00, 0x48};

// The fake thread start: records its call and returns a node entry whose +8 points at a data slot.
struct FakeStart {
  int calls = 0;
  std::uint32_t node = 0;
  void* script = nullptr;
  std::uint64_t data_slot = 0;
  std::array<std::uint8_t, kEntrySize> entry{};
  void Reset() {
    calls = 0;
    node = 0;
    script = nullptr;
    data_slot = 0;
    entry.fill(0);
    const std::uintptr_t slot = reinterpret_cast<std::uintptr_t>(&data_slot);
    std::memcpy(entry.data() + kEntryDataSlotAt, &slot, sizeof slot);
  }
};
FakeStart g_start;

std::uint8_t* FakeStartFn(void* script, std::uint32_t node) {
  ++g_start.calls;
  g_start.node = node;
  g_start.script = script;
  return g_start.entry.data();
}

// The fake script: its counters pointer at kScriptCountersAt, the counters are kCounterCount dwords.
struct FakeScript {
  std::array<std::uint8_t, kScriptSize> script{};
  std::array<std::int32_t, kCounterCount> counters{};
  FakeScript() {
    const std::uintptr_t c = reinterpret_cast<std::uintptr_t>(counters.data());
    std::memcpy(script.data() + kScriptCountersAt, &c, sizeof c);
  }
  void* ptr() { return script.data(); }
};

// A fake event: word 0 the symbol, and the words the thread data copies.
std::array<std::uint64_t, kEventWordCount> MakeEvent(std::uint64_t symbol) {
  std::array<std::uint64_t, kEventWordCount> e{};
  e[0] = symbol;
  e[kEventThreadWordIndex] = kTestEventWord;
  e[kEventThreadWord2Index] = kTestEventWord2;
  e[kEventDataIndex] = kTestEventData;
  return e;
}

template <typename T>
T ReadAt(const std::uint8_t* at) {
  T v{};
  std::memcpy(&v, at, sizeof v);
  return v;
}

// The stock functions the Run* tests call through: they record what they were given.
struct StockRecord {
  int handler_calls = 0;
  std::uint64_t handler_event = 0;
  int query_calls = 0;
  std::array<const void*, announcer::kGameEventCount> game{};
  std::array<std::uint8_t, announcer::kGameEventCount> marks{};
  std::uint32_t read_event = 0;
  int read_calls = 0;
};
StockRecord g_stock;

void StockHandlerFn(void* /*script*/, const std::uint64_t* event) {
  ++g_stock.handler_calls;
  g_stock.handler_event = event[0];
}

std::uint32_t StockQueryFn(std::uint64_t /*key*/, const void** table, std::uint32_t* count) {
  ++g_stock.query_calls;
  for (std::size_t i = 0; i < g_stock.game.size(); ++i) g_stock.game[i] = &g_stock.marks[i];
  *table = g_stock.game.data();
  *count = static_cast<std::uint32_t>(announcer::kGameEventCount);
  return kStockQueryAnswer;
}

std::uint64_t StockReadFn(std::uint8_t* output, void* /*node*/) {
  ++g_stock.read_calls;
  g_stock.read_event = ReadAt<std::uint32_t>(output + kOutputEventAt);
  return kStockReadResult;
}

// The one log sink the tests read (the boot sink Log() writes to in the test binary).
std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const std::string& needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

// The jump the table wrote at a hooked entry: its target, or 0 when the entry holds no jump.
std::uintptr_t JumpTarget(const std::uint8_t* at) {
  if (at[0] == kJumpRelativeOpcode) {
    std::int32_t rel = 0;
    std::memcpy(&rel, at + 1, sizeof rel);
    return reinterpret_cast<std::uintptr_t>(at + kJumpRelativeLength) + static_cast<std::intptr_t>(rel);
  }
  if (at[0] == kJumpIndirectOpcode0 && at[1] == kJumpIndirectOpcode1) {
    return ReadAt<std::uintptr_t>(at + kJumpAbsoluteTargetAt);
  }
  return 0;
}

// Where an entry finally lands: MinHook writes a relative jump to a relay in reach, and the relay jumps to the detour.
constexpr int kMaxJumpHops = 2;
std::uintptr_t FinalTarget(const std::uint8_t* at) {
  std::uintptr_t target = reinterpret_cast<std::uintptr_t>(at);
  for (int hop = 0; hop < kMaxJumpHops; ++hop) {
    const std::uintptr_t next = JumpTarget(reinterpret_cast<const std::uint8_t*>(target));
    if (next == 0) break;
    target = next;
  }
  return target;
}

// The label names a published set carries, and a shadow whose one record is the bank (tagged with `user`) or another resource.
const std::vector<std::string> kLabels = {"lobby_posters", announcer::kRecordsLabel};

std::unique_ptr<nevr_shadow::Shadow> MakeShadow(bool bank, std::uint32_t user) {
  nevr_shadow::Input input;
  nevr_shadow::Record rec;
  rec.type = bank ? announcer::kBankType : kOtherRecordType;
  rec.name = bank ? announcer::kBankName : kOtherRecordName;
  rec.size = kTestRecordSize;
  rec.user = user;
  input.records.push_back(rec);
  nevr_shadow::Chunk chunk;
  chunk.decompressed = kTestChunkBytes;
  input.chunks.push_back(chunk);
  input.handles.push_back(kTestPackageHandle);
  std::unique_ptr<nevr_shadow::Shadow> shadow;
  EXPECT_EQ(nevr_shadow::Shadow::Build(input, &shadow), nevr_shadow::Status::kOk);
  return shadow;
}

// Where a trampoline jumps back to: the first jump in its leading bytes that lands within `window` bytes after `entry`.
std::uintptr_t BackJumpInto(const void* trampoline, const std::uint8_t* entry, int window) {
  const auto* bytes = static_cast<const std::uint8_t*>(trampoline);
  for (int i = 0; i < kMaxBackJumpScan; ++i) {
    const std::uintptr_t to = JumpTarget(bytes + i);
    const auto lo = reinterpret_cast<std::uintptr_t>(entry);
    if (to > lo && to <= lo + static_cast<std::uintptr_t>(window)) return to;
  }
  return 0;
}

}  // namespace

TEST(AnnouncerHashes, EventSymbolsMatchTheStockImmediate) {
  EXPECT_EQ(EchoVR::CSymbol64Hash("evt_vo_hra_equip_ability_sensor"), kAbilitySensor);
  EXPECT_EQ(EchoVR::CSymbol64Hash("evt_vo_hra_equip_weapon_chain"), kStockChainEvent);
}

TEST(AnnouncerHashes, WwiseIdsMatchFixVo) {
  EXPECT_EQ(announcer::Fnv1(kPulsarWwiseName), announcer::kPulsarEvent);
  EXPECT_EQ(announcer::Fnv1(kPulsarWwiseNameUpper), announcer::kPulsarEvent);
}

TEST(AnnouncerHashes, Fnv1IsTheFnv1Constants) {
  EXPECT_EQ(announcer::Fnv1(""), announcer::kFnvOffsetBasis);
  const std::uint32_t expected = (announcer::kFnvOffsetBasis * announcer::kFnvPrime) ^ static_cast<std::uint8_t>('a');
  EXPECT_EQ(announcer::Fnv1("a"), expected);
  EXPECT_EQ(announcer::Fnv1("A"), expected);
}

TEST(AnnouncerLines, EachOfTheSevenIsFoundAndNoOtherIs) {
  for (std::size_t i = 0; i < announcer::kLineCount; ++i) {
    EXPECT_EQ(announcer::FindLine(announcer::kLines[i].game_event), static_cast<int>(i)) << announcer::kLines[i].name;
  }
  EXPECT_EQ(announcer::FindLine(kAbilitySensor), -1);
  EXPECT_EQ(announcer::FindLine(0), -1);
}

TEST(AnnouncerLines, BlazarIsTheStockChainEventAndTheOthersHaveNoStockListener) {
  EXPECT_STREQ(announcer::kLines[kBlazarIndex].name, "blazar");
  EXPECT_EQ(announcer::kLines[kBlazarIndex].game_event, kStockChainEvent);
  EXPECT_EQ(announcer::kLines[kBlazarIndex].wwise, kStockChainWwise);
  EXPECT_STREQ(announcer::kLines[kFirstFakeLine].game_name, kTranslocatorGameName);
  EXPECT_EQ(announcer::kAddedNames, kLinesWithoutAListener);
}

TEST(AnnouncerHandler, IdleLineStartsPulsarWithTheLineAsTheOverride) {
  FakeScript s;
  g_start.Reset();
  std::atomic<std::uint32_t> override_event{0};
  auto e = MakeEvent(announcer::kLines[kFirstFakeLine].game_event);
  EXPECT_TRUE(announcer::HandleEvent(s.ptr(), e.data(), &FakeStartFn, override_event));
  EXPECT_EQ(g_start.calls, 1);
  EXPECT_EQ(g_start.node, announcer::kPulsarNode);
  EXPECT_EQ(g_start.script, s.ptr());
  EXPECT_EQ(override_event.load(), announcer::kLines[kFirstFakeLine].wwise);
  // the thread data the stock listener writes: *(node+8) <- event+0x30, node+0x50 <- event+0x18, node+0x58 <- event+0x28
  EXPECT_EQ(g_start.data_slot, kTestEventData);
  EXPECT_EQ(ReadAt<std::uint64_t>(g_start.entry.data() + kEntryWordAt), kTestEventWord);
  EXPECT_EQ(ReadAt<std::uint64_t>(g_start.entry.data() + kEntryWord2At), kTestEventWord2);
  // the PULSAR busy counter goes up by one, and the node's +0x70 points at it
  EXPECT_EQ(s.counters[kCounterIndex], 1);
  EXPECT_EQ(ReadAt<std::uint64_t>(g_start.entry.data() + kEntryCounterAddrAt),
            reinterpret_cast<std::uintptr_t>(&s.counters[kCounterIndex]));
}

TEST(AnnouncerHandler, EveryLineStartsPulsarWithItsOwnOverride) {
  for (std::size_t i = 0; i < announcer::kLineCount; ++i) {
    FakeScript s;
    g_start.Reset();
    std::atomic<std::uint32_t> override_event{0};
    auto e = MakeEvent(announcer::kLines[i].game_event);
    EXPECT_TRUE(announcer::HandleEvent(s.ptr(), e.data(), &FakeStartFn, override_event)) << announcer::kLines[i].name;
    EXPECT_EQ(override_event.load(), announcer::kLines[i].wwise) << announcer::kLines[i].name;
    EXPECT_EQ(g_start.calls, 1) << announcer::kLines[i].name;
  }
}

TEST(AnnouncerHandler, BusyPulsarTakesTheEventAndStartsNothing) {
  FakeScript s;
  s.counters[kCounterIndex] = announcer::kBusy;
  g_start.Reset();
  std::atomic<std::uint32_t> override_event{kUntouchedOverride};
  auto e = MakeEvent(announcer::kLines[kBlazarIndex].game_event);
  EXPECT_TRUE(announcer::HandleEvent(s.ptr(), e.data(), &FakeStartFn, override_event));
  EXPECT_EQ(g_start.calls, 0);
  EXPECT_EQ(override_event.load(), kUntouchedOverride);
  EXPECT_EQ(s.counters[kCounterIndex], announcer::kBusy);
}

TEST(AnnouncerHandler, OtherEventsGoToTheStockHandler) {
  FakeScript s;
  g_start.Reset();
  std::atomic<std::uint32_t> override_event{kUntouchedOverride};
  auto e = MakeEvent(kAbilitySensor);
  EXPECT_FALSE(announcer::HandleEvent(s.ptr(), e.data(), &FakeStartFn, override_event));
  EXPECT_EQ(g_start.calls, 0);
  EXPECT_EQ(override_event.load(), kUntouchedOverride);
  EXPECT_EQ(s.counters[kCounterIndex], 0);
}

TEST(AnnouncerRunHandler, AStockEventGoesToTheStockHandlerOnce) {
  g_stock = StockRecord{};
  FakeScript s;
  g_start.Reset();
  std::atomic<std::uint32_t> override_event{kUntouchedOverride};
  auto e = MakeEvent(kAbilitySensor);
  announcer::RunHandler(s.ptr(), e.data(), &FakeStartFn, override_event, &StockHandlerFn);
  EXPECT_EQ(g_stock.handler_calls, 1);
  EXPECT_EQ(g_stock.handler_event, kAbilitySensor);
  EXPECT_EQ(g_start.calls, 0);
  EXPECT_EQ(override_event.load(), kUntouchedOverride);
}

TEST(AnnouncerRunHandler, ALineIsTakenAndTheStockHandlerDoesNotRun) {
  g_stock = StockRecord{};
  FakeScript s;
  g_start.Reset();
  std::atomic<std::uint32_t> override_event{0};
  auto e = MakeEvent(announcer::kLines[1].game_event);
  announcer::RunHandler(s.ptr(), e.data(), &FakeStartFn, override_event, &StockHandlerFn);
  EXPECT_EQ(g_stock.handler_calls, 0);
  EXPECT_EQ(g_start.calls, 1);
  EXPECT_EQ(override_event.load(), announcer::kLines[1].wwise);
}

TEST(AnnouncerQuery, AppendsOurSixNamesAfterTheGamesTwenty) {
  std::array<const void*, announcer::kGameEventCount> game{};
  std::array<std::uint8_t, announcer::kGameEventCount> marks{};
  for (std::size_t i = 0; i < game.size(); ++i) game[i] = &marks[i];
  const void* table = game.data();
  std::uint32_t count = static_cast<std::uint32_t>(announcer::kGameEventCount);
  std::array<const void*, announcer::kTableSize> scratch{};
  EXPECT_TRUE(announcer::ExtendEvents(announcer::kEventsKey, kStockQueryAnswer, &table, &count, scratch.data()));
  EXPECT_EQ(count, announcer::kTableSize);
  EXPECT_EQ(table, static_cast<const void*>(scratch.data()));
  for (std::size_t i = 0; i < game.size(); ++i) EXPECT_EQ(scratch[i], game[i]);
  // ours: the six lines whose event the game does not list (blazar's chain name is listed already)
  for (std::size_t i = 0; i < announcer::kAddedNames; ++i) {
    EXPECT_EQ(std::string(static_cast<const char*>(scratch[announcer::kGameEventCount + i])),
              announcer::kLines[i].game_name);
  }
}

TEST(AnnouncerQuery, OtherKeysAndAFailedQueryLeaveTheTableAlone) {
  std::array<const void*, announcer::kGameEventCount> game{};
  std::uint32_t count = static_cast<std::uint32_t>(announcer::kGameEventCount);
  std::array<const void*, announcer::kTableSize> scratch{};
  const void* table = game.data();
  EXPECT_FALSE(announcer::ExtendEvents(announcer::kEventsKey + kOtherKeyDelta, kStockQueryAnswer, &table, &count,
                                       scratch.data()));
  EXPECT_FALSE(announcer::ExtendEvents(announcer::kEventsKey, kGameTableEntryFill, &table, &count, scratch.data()));
  EXPECT_EQ(table, static_cast<const void*>(game.data()));
  EXPECT_EQ(count, announcer::kGameEventCount);
}

TEST(AnnouncerRunQuery, TheEventsKeyIsExtendedAndAnotherKeyIsNot) {
  g_stock = StockRecord{};
  std::array<const void*, announcer::kTableSize> scratch{};
  const void* table = nullptr;
  std::uint32_t count = 0;
  const std::uint32_t answer = announcer::RunQuery(announcer::kEventsKey, &table, &count, &StockQueryFn, scratch.data());
  EXPECT_EQ(answer, kStockQueryAnswer);
  EXPECT_EQ(g_stock.query_calls, 1);
  EXPECT_EQ(table, static_cast<const void*>(scratch.data()));
  EXPECT_EQ(count, announcer::kTableSize);
  EXPECT_NE(g_stock.game[0], nullptr);
  EXPECT_EQ(scratch[0], g_stock.game[0]);

  g_stock = StockRecord{};
  table = nullptr;
  count = 0;
  announcer::RunQuery(announcer::kEventsKey + kOtherKeyDelta, &table, &count, &StockQueryFn, scratch.data());
  EXPECT_EQ(g_stock.query_calls, 1);
  EXPECT_EQ(table, static_cast<const void*>(g_stock.game.data()));
  EXPECT_EQ(count, announcer::kGameEventCount);
}

TEST(AnnouncerRead, PulsarIsSubstitutedBeforeTheStockReadSeesIt) {
  g_stock = StockRecord{};
  std::array<std::uint8_t, kOutputSize> out{};
  std::memcpy(out.data() + kOutputEventAt, &announcer::kPulsarEvent, sizeof announcer::kPulsarEvent);
  std::atomic<std::uint32_t> override_event{kSentinelOverride};
  EXPECT_EQ(announcer::RunRead(out.data(), nullptr, override_event, &StockReadFn), kStockReadResult);
  EXPECT_EQ(g_stock.read_calls, 1);
  EXPECT_EQ(g_stock.read_event, kSentinelOverride);
  EXPECT_EQ(override_event.load(), 0u);
}

TEST(AnnouncerRead, PulsarWithoutAnOverridePlaysItsOwnEvent) {
  g_stock = StockRecord{};
  std::array<std::uint8_t, kOutputSize> out{};
  std::memcpy(out.data() + kOutputEventAt, &announcer::kPulsarEvent, sizeof announcer::kPulsarEvent);
  std::atomic<std::uint32_t> override_event{0};
  announcer::RunRead(out.data(), nullptr, override_event, &StockReadFn);
  EXPECT_EQ(g_stock.read_event, announcer::kPulsarEvent);
  EXPECT_EQ(override_event.load(), 0u);
}

TEST(AnnouncerRead, EveryOtherNodeEventReachesTheStockReadUntouched) {
  g_stock = StockRecord{};
  std::array<std::uint8_t, kOutputSize> out{};
  std::memcpy(out.data() + kOutputEventAt, &kOtherNodeEvent, sizeof kOtherNodeEvent);
  std::atomic<std::uint32_t> override_event{kSentinelOverride};
  announcer::RunRead(out.data(), nullptr, override_event, &StockReadFn);
  EXPECT_EQ(g_stock.read_event, kOtherNodeEvent);
  EXPECT_EQ(override_event.load(), kSentinelOverride);
}

TEST(AnnouncerRead, TheOverrideIsTakenOnce) {
  std::array<std::uint8_t, kOutputSize> out{};
  std::memcpy(out.data() + kOutputEventAt, &announcer::kPulsarEvent, sizeof announcer::kPulsarEvent);
  std::atomic<std::uint32_t> override_event{kSentinelOverride};
  announcer::PlayEvent(out.data(), override_event);
  EXPECT_EQ(ReadAt<std::uint32_t>(out.data() + kOutputEventAt), kSentinelOverride);
  std::memcpy(out.data() + kOutputEventAt, &announcer::kPulsarEvent, sizeof announcer::kPulsarEvent);
  announcer::PlayEvent(out.data(), override_event);
  EXPECT_EQ(ReadAt<std::uint32_t>(out.data() + kOutputEventAt), announcer::kPulsarEvent);
}

TEST(AnnouncerBankGate, OnlyTheBankRecordUnderTheCombatLabelCounts) {
  EXPECT_TRUE(announcer::HoldsBank(MakeShadow(true, kCombatLabelIndex).get(), kLabels));
  EXPECT_FALSE(announcer::HoldsBank(MakeShadow(false, kCombatLabelIndex).get(), kLabels)) << "another record";
  EXPECT_FALSE(announcer::HoldsBank(MakeShadow(true, kPostersLabelIndex).get(), kLabels)) << "the bank under another label";
  EXPECT_FALSE(announcer::HoldsBank(MakeShadow(true, kCombatLabelIndex).get(), {})) << "a tag with no label name";
  EXPECT_FALSE(announcer::HoldsBank(nullptr, kLabels));
}

TEST(AnnouncerFree, TheScriptFreeClearsAnUnplayedOverride) {
  announcer::CurrentOverride().store(kSentinelOverride);
  announcer::OnFreed(announcer::kScript);
  EXPECT_EQ(announcer::CurrentOverride().load(), 0u);
}

// The rows through the script hook table, on a fake image: the stock bytes at the three entries, the table's own install.
class AnnouncerInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    announcer::ResetForTest();
    sh::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kTestFakeImageSize, MEM_RESERVE | MEM_COMMIT,
                                                    PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    std::memset(base_, kFakeImageFill, kTestFakeImageSize);
    std::memcpy(base_ + kImageHandlerRva, kStockHandlerBytes, sizeof kStockHandlerBytes);
    std::memcpy(base_ + kImageQueryRva, kStockQueryBytes, sizeof kStockQueryBytes);
    std::memcpy(base_ + kImageReadRva, kStockReadBytes, sizeof kStockReadBytes);
  }
  void TearDown() override {
    MH_RemoveHook(base_ + kImageHandlerRva);
    MH_RemoveHook(base_ + kImageQueryRva);
    MH_RemoveHook(base_ + kImageReadRva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    announcer::ResetForTest();
    SetBootLogSink(nullptr);
  }
  // The boot set holds the bank; shadows are kept for the test's life.
  void MountWithRecords() {
    shadows_.push_back(MakeShadow(true, kCombatLabelIndex));
    announcer::OnPublished(shadows_.back().get(), kLabels, kBootSet);
  }
  std::vector<std::unique_ptr<nevr_shadow::Shadow>> shadows_;
  std::uint8_t* base_ = nullptr;
};

TEST_F(AnnouncerInstalled, TheStockBytesAreTheHeaderPins) {
  EXPECT_EQ(std::vector<std::uint8_t>(std::begin(announcer::kHandlerPin), std::end(announcer::kHandlerPin)),
            std::vector<std::uint8_t>(std::begin(kStockHandlerBytes), std::end(kStockHandlerBytes)));
  EXPECT_EQ(std::vector<std::uint8_t>(std::begin(announcer::kQueryPin), std::end(announcer::kQueryPin)),
            std::vector<std::uint8_t>(std::begin(kStockQueryBytes), std::end(kStockQueryBytes)));
  EXPECT_EQ(std::vector<std::uint8_t>(std::begin(announcer::kReadPin), std::end(announcer::kReadPin)),
            std::vector<std::uint8_t>(std::begin(kStockReadBytes), std::end(kStockReadBytes)));
}

TEST_F(AnnouncerInstalled, ALabelWithoutTheBankRecordRegistersNothing) {
  shadows_.push_back(MakeShadow(false, kCombatLabelIndex));
  announcer::OnPublished(shadows_.back().get(), kLabels, kBootSet);
  EXPECT_FALSE(announcer::RegisteredForTest());
  EXPECT_FALSE(announcer::DeferredForTest());
  sh::OnLoaded(announcer::kScript, base_);
  EXPECT_EQ(sh::HookedModules(), 0u);
  EXPECT_EQ(JumpTarget(base_ + kImageHandlerRva), 0u);
}

TEST_F(AnnouncerInstalled, TheBankOnlyInAFetchedSetRegistersNothingAndSaysWhen) {
  shadows_.push_back(MakeShadow(true, kCombatLabelIndex));
  announcer::OnPublished(shadows_.back().get(), kLabels, kFetchedSet);
  announcer::OnPublished(shadows_.back().get(), kLabels, kFetchedSet);
  EXPECT_FALSE(announcer::RegisteredForTest());
  EXPECT_TRUE(announcer::DeferredForTest());
  EXPECT_EQ(Count("announcer callouts deferred reason=bank_arrived_after_boot"), 1) << "one line, not one a set";
  EXPECT_NE(Count("start at the next launch"), 0);
  sh::OnLoaded(announcer::kScript, base_);
  EXPECT_EQ(sh::HookedModules(), 0u);
  EXPECT_EQ(JumpTarget(base_ + kImageHandlerRva), 0u);
}

TEST_F(AnnouncerInstalled, TheBankInTheBootSetRegistersAndALaterFetchedSetSaysNothing) {
  MountWithRecords();
  EXPECT_TRUE(announcer::RegisteredForTest());
  announcer::OnPublished(shadows_.back().get(), kLabels, kFetchedSet);
  EXPECT_FALSE(announcer::DeferredForTest());
  EXPECT_EQ(Count("announcer callouts deferred"), 0);
}

TEST_F(AnnouncerInstalled, MountingTheRecordsInstallsTheThreeRowsOnTheirEntries) {
  MountWithRecords();
  EXPECT_TRUE(announcer::RegisteredForTest());
  sh::OnLoaded(announcer::kScript, base_);
  EXPECT_EQ(sh::HookedModules(), 1u);
  EXPECT_EQ(Count("script hooks installed script=7c10e713aec2f12b"), 1);
  EXPECT_EQ(Count("hooks=" + std::to_string(announcer::kHookRows) + " slots=0 pins=0"), 1);
  EXPECT_EQ(Count("announcer callouts registered script=7c10e713aec2f12b hooks=" + std::to_string(announcer::kHookRows)),
            1);
  // each entry jumps to its own detour
  EXPECT_EQ(FinalTarget(base_ + kImageHandlerRva), reinterpret_cast<std::uintptr_t>(&announcer::HandlerDetour));
  EXPECT_EQ(FinalTarget(base_ + kImageQueryRva), reinterpret_cast<std::uintptr_t>(&announcer::QueryDetour));
  EXPECT_EQ(FinalTarget(base_ + kImageReadRva), reinterpret_cast<std::uintptr_t>(&announcer::ReadDetour));
  // each detour calls its own stock function: each original is the trampoline of its own entry, which jumps back to just
  // after that entry, so two originals crossed between rows fail
  const announcer::OriginalsForTest originals = announcer::Originals();
  EXPECT_NE(BackJumpInto(originals.handler, base_ + kImageHandlerRva, kMaxCopiedPrologue), 0u);
  EXPECT_NE(BackJumpInto(originals.query, base_ + kImageQueryRva, kMaxCopiedPrologue), 0u);
  EXPECT_NE(BackJumpInto(originals.read, base_ + kImageReadRva, kMaxCopiedPrologue), 0u);
  EXPECT_EQ(BackJumpInto(originals.handler, base_ + kImageReadRva, kMaxCopiedPrologue), 0u);
  EXPECT_EQ(BackJumpInto(originals.read, base_ + kImageHandlerRva, kMaxCopiedPrologue), 0u);
  EXPECT_EQ(BackJumpInto(originals.query, base_ + kImageHandlerRva, kMaxCopiedPrologue), 0u);
}

TEST_F(AnnouncerInstalled, NothingIsBoundBeforeTheScriptLoads) {
  MountWithRecords();
  const announcer::OriginalsForTest originals = announcer::Originals();
  EXPECT_EQ(originals.handler, nullptr);
  EXPECT_EQ(originals.query, nullptr);
  EXPECT_EQ(originals.read, nullptr);
}

TEST_F(AnnouncerInstalled, RegisteringTwiceAddsTheRowsOnce) {
  MountWithRecords();
  MountWithRecords();
  announcer::Register();
  sh::OnLoaded(announcer::kScript, base_);
  EXPECT_EQ(Count("script hooks installed script=7c10e713aec2f12b"), 1);
  EXPECT_EQ(Count("announcer callouts registered"), 1);
}

TEST_F(AnnouncerInstalled, AChangedPinRefusesTheWholeScript) {
  base_[kImageReadRva + kCorruptPinIndex] ^= kCorruptPinMask;
  MountWithRecords();
  sh::OnLoaded(announcer::kScript, base_);
  EXPECT_EQ(sh::HookedModules(), 0u);
  EXPECT_EQ(Count("script hooks refused script=7c10e713aec2f12b"), 1);
  EXPECT_EQ(JumpTarget(base_ + kImageHandlerRva), 0u);
}

TEST_F(AnnouncerInstalled, TheScriptFreeClearsTheOverrideAndTakesTheHooksOut) {
  MountWithRecords();
  sh::OnLoaded(announcer::kScript, base_);
  announcer::CurrentOverride().store(kSentinelOverride);
  sh::OnUnloading(announcer::kScript, base_);
  EXPECT_EQ(announcer::CurrentOverride().load(), 0u);
  EXPECT_EQ(sh::HookedModules(), 0u);
  EXPECT_EQ(Count("script hooks removed script=7c10e713aec2f12b"), 1);
}
