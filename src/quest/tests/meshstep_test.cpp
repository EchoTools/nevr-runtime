// Host test of the mesh-list table-step module (src/quest/combat/meshstep.cpp, #2177): the real handler through the real thunk, installed by the
// real GotHook into the JUMP_SLOT of a libr15 image built in memory, and called the way the game's PLT stub calls it: through the slot. The game
// side is a model: the pinned words at their vaddrs, the two vtable data words, fake helpers that record the calls the stock order makes, and a
// stand-in for the game's constructor that lays a recognisable object down.
//
// What it pins: in a combat match the object is the stock constructor's in the stock order with growth step 1; in an Echo Arena level, in a
// lobby arena players use, in the combat lobby, in a transition out of combat, with nothing loaded and with no game known the game's own
// constructor runs once and the object is exactly what it made (arena pays nothing it did not pay before, and the scope is read once per
// construction); each refusal (a pinned word, a vtable word, a helper, a foreign slot word, the build, the switch, a second step) leaves the
// slot alone; the plugin's kill file is a Warning; the golden lines.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/meshstep.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/table_step_fixture.h"

namespace {

namespace ms = nevr_quest::combat::meshstep;
namespace ts = nevr_quest::combat::table_step;
using namespace table_step_test;

constexpr std::uintptr_t kTableOffsets[] = {0x2b0, 0x2e8, 0x320, 0x358, 0x390, 0x3c8, 0x400, 0x438, 0x470, 0x4a8, 0x4e0};
constexpr std::uint64_t kCurrent = 0xC0FFEE;
void* const kCurrentPtr = reinterpret_cast<void*>(0xA110C);
constexpr std::size_t kObjectBytes = 0x800;

// ---- the fake engine ------------------------------------------------------------------------------------------------------------

unsigned g_listCalls = 0;
void* g_listSelf = nullptr;
unsigned g_blockCalls = 0;
void* g_blockSelf[11] = {};
std::uint64_t g_blockA[11] = {};
std::uint64_t g_blockB[11] = {};
void* g_blockAlloc[11] = {};
unsigned g_stockCalls = 0;
void* g_stockSelf = nullptr;
const void* g_stockInit = nullptr;

// What the object's first word held at the two points where the stock order makes it visible: when Current() is called (the CPresence vtable is
// stored, the mesh one is not yet) and when the list data is constructed (the mesh vtable is stored).
std::uint8_t* g_building = nullptr;
std::uint64_t g_vptrAtCurrent = 0;
std::uint64_t g_vptrAtListData = 0;

std::uint64_t FakeCurrent() {
  if (g_building != nullptr) std::memcpy(&g_vptrAtCurrent, g_building, sizeof g_vptrAtCurrent);
  return kCurrent;
}
void* FakeCurrentPtr() { return kCurrentPtr; }
void FakeListDataCtor(void* self) {
  if (g_building != nullptr) std::memcpy(&g_vptrAtListData, g_building, sizeof g_vptrAtListData);
  ++g_listCalls;
  g_listSelf = self;
}
void FakeMemBlockCtor(void* self, std::uint64_t a, std::uint64_t b, void* alloc) {
  std::memset(self, 0xEE, 0x38);  // the constructor initialises the whole block, so a step stored before it is lost
  if (g_blockCalls < 11) {
    g_blockSelf[g_blockCalls] = self;
    g_blockA[g_blockCalls] = a;
    g_blockB[g_blockCalls] = b;
    g_blockAlloc[g_blockCalls] = alloc;
  }
  ++g_blockCalls;
}
// The game's constructor: a recognisable object, with the stock step 32 in the eleven blocks.
extern "C" __attribute__((noinline)) void FakeStockCtor(void* self, const void* init) {
  ++g_stockCalls;
  g_stockSelf = self;
  g_stockInit = init;
  if (self == nullptr) return;  // the stand-in for the game's own handling of a null object
  auto* o = static_cast<std::uint8_t*>(self);
  std::memset(o, 0x5a, kObjectBytes);
  for (const std::uintptr_t off : kTableOffsets) {
    const std::uint64_t stock = 32;
    std::memcpy(o + off + 0x20, &stock, sizeof stock);
  }
}

// A function the game does not own, in an executable mapping: a slot that holds it is another hook's (or another build's), not the constructor.
extern "C" __attribute__((noinline)) void ForeignCtor(void*, const void*) {}

ms::Fns FakeFns() {
  ms::Fns f{};
  f.current = &FakeCurrent;
  f.currentPtr = &FakeCurrentPtr;
  f.listDataCtor = &FakeListDataCtor;
  f.memblockCtor = &FakeMemBlockCtor;
  return f;
}
void ResetFakes() {
  g_listCalls = g_blockCalls = g_stockCalls = 0;
  g_listSelf = g_stockSelf = nullptr;
  g_stockInit = nullptr;
  g_building = nullptr;
  g_vptrAtCurrent = g_vptrAtListData = 0;
}

// The object the game's constructor makes alone, from the same uninitialised-memory pattern Construct() uses: what a stray write must not change.
std::uint8_t* ReferenceObject(const std::uint64_t (&init)[3]) {
  auto* ref = static_cast<std::uint8_t*>(std::malloc(kObjectBytes));
  QCHECK(ref != nullptr);
  if (ref == nullptr) return nullptr;
  std::memset(ref, 0xA5, kObjectBytes);
  const unsigned calls = g_stockCalls;
  FakeStockCtor(ref, init);
  g_stockCalls = calls;
  return ref;
}

// ---- the image ------------------------------------------------------------------------------------------------------------------

ms::Pins g_pins = ms::kPins;
ms::Fns g_fns = FakeFns();
using CtorFn = void (*)(void*, const void*);

std::uint64_t Word64(const std::uint8_t* b, std::size_t off) {
  std::uint64_t v = 0;
  std::memcpy(&v, b + off, sizeof v);
  return v;
}

// A libr15 holding the pinned words and the two vtable data words, the slot holding the game's constructor (the stand-in).
void Build(bool foreignSlot = false) {
  g_pins = ms::kPins;
  g_image.Build({{ms::kSymbol, g_pins.slot, false, false, g_pins.ctor}});
  // base + ctor is the stand-in's address: the real constructor is not in the image, so the pin holds the stand-in's offset.
  g_pins.ctor = reinterpret_cast<std::uintptr_t>(&FakeStockCtor) - g_image.Base();
  PutWords(ms::kWords);
  g_image.Put<std::uintptr_t>(g_pins.vtblPresence, 0x1000);  // the stored word; the reconstruction adds 0x10
  g_image.Put<std::uintptr_t>(g_pins.vtblMesh, 0x2000);
  *g_image.Slot(g_pins.slot) = foreignSlot ? reinterpret_cast<void*>(&ForeignCtor) : reinterpret_cast<void*>(&FakeStockCtor);
}
CtorFn SlotFn() { return reinterpret_cast<CtorFn>(*g_image.Slot(g_pins.slot)); }
bool Hooked() { return *g_image.Slot(g_pins.slot) == ms::Thunk::EntryAddress(); }

void Begin1() {
  Begin();
  ms::ResetForTest();
  ResetFakes();
  ms::SetImageLookupForTest(&Lookup);
  ms::SetPinsForTest(&g_pins);
  g_fns = FakeFns();
  ms::SetFnsForTest(&g_fns);
}
void End1() {
  ms::ResetForTest();
  ms::SetImageLookupForTest(nullptr);
  ms::SetPinsForTest(nullptr);
  ms::SetFnsForTest(nullptr);
  End();
}

// Calls the slot as the game's stub does, with a fresh object; the caller frees it.
std::uint8_t* Construct(std::uint64_t (&init)[3]) {
  auto* obj = static_cast<std::uint8_t*>(std::malloc(kObjectBytes));
  QCHECK(obj != nullptr);
  if (obj == nullptr) return nullptr;
  std::memset(obj, 0xA5, kObjectBytes);  // the game allocates uninitialised memory: every store the constructor makes is visible
  g_building = obj;
  SlotFn()(obj, init);
  g_building = nullptr;
  return obj;
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfAndDisassemblyPins() {
  const ms::Pins& d = ms::kPins;
  QCHECK(d.ctor == 0x18677cc && d.slot == 0x36e5548 && d.current == 0xf28430 && d.listDataCtor == 0xef9c70);
  QCHECK(d.currentPtr == 0xefbc50 && d.memblockCtor == 0xeedec0 && d.vtblPresence == 0x371f860 && d.vtblMesh == 0x370c7d8);
  QCHECK(ms::kWordCount == 33);
  // The PLT stub the callers reach the constructor through: `adrp x16,0x36e5000; ldr x17,[x16,#0x548]` at 0xf1bcc0.
  QCHECK(ms::kWords[0].vaddr == 0xf1bcc0 && ms::kWords[0].word == 0xd0013e50);
  QCHECK(ms::kWords[1].vaddr == 0xf1bcc4 && ms::kWords[1].word == 0xf942a611);
  QCHECK(ms::kWords[8].vaddr == 0x186784c && ms::kWords[8].word == 0x52800414);  // mov w20,#0x20
  QCHECK(ms::kWords[ms::kWordCount - 1].vaddr == 0x18679b4 && ms::kWords[ms::kWordCount - 1].word == 0x97da1943);
  QCHECK(std::strcmp(ms::kSymbol, "_ZN10NRadEngine18CGMeshListResourceC2ERKNS_13CPresenceInitE") == 0);
}

// What the pinned words say, decoded: the PLT stub's adrp/ldr name the slot, the two adrp/ldr pairs of the constructor name the two vtable data words,
// and the `bl` words call exactly the four helpers (the one Current, the one list-data constructor, and eleven each of CurrentPtr and the CMemBlock
// constructor, one for each table). The words are the file's (got_pinned_test checks that); this ties the addresses the module uses to them.
std::uint32_t PinnedWord(std::uint64_t vaddr) {
  for (const auto& w : ms::kWords) {
    if (w.vaddr == vaddr) return w.word;
  }
  QCHECK(false);
  return 0;
}
// adrp xN, <page>: the page's address from the word at `pc`.
std::uint64_t AdrpPage(std::uint64_t pc, std::uint32_t word) {
  QCHECK((word & 0x9f000000U) == 0x90000000U);
  std::int64_t imm = static_cast<std::int64_t>(((word >> 5) & 0x7ffffU) << 2 | ((word >> 29) & 3U));
  if ((imm & (1 << 20)) != 0) imm -= (1 << 21);
  return (pc & ~0xfffULL) + static_cast<std::uint64_t>(imm << 12);
}
// ldr xN, [xM, #imm] (64-bit, unsigned offset): the byte offset.
std::uint64_t LdrOffset(std::uint32_t word) {
  QCHECK((word & 0xffc00000U) == 0xf9400000U);
  return static_cast<std::uint64_t>((word >> 10) & 0xfffU) * 8;
}
void ThePinnedWordsNameTheSlotTheVtablesAndTheFourHelpers() {
  const ms::Pins& d = ms::kPins;
  // The stub: adrp x16,<slot's page>; ldr x17,[x16,#<slot's offset in the page>].
  QCHECK(AdrpPage(0xf1bcc0, PinnedWord(0xf1bcc0)) + LdrOffset(PinnedWord(0xf1bcc4)) == d.slot);
  // The constructor's vtable loads: adrp/ldr pairs at 0x18677d4/0x18677d8 (CPresence) and 0x1867800/0x1867808 (CGMeshListResource).
  QCHECK(AdrpPage(0x18677d4, PinnedWord(0x18677d4)) + LdrOffset(PinnedWord(0x18677d8)) == d.vtblPresence);
  QCHECK(AdrpPage(0x1867800, PinnedWord(0x1867800)) + LdrOffset(PinnedWord(0x1867808)) == d.vtblMesh);
  unsigned current = 0, listData = 0, currentPtr = 0, memblock = 0, other = 0;
  for (const auto& w : ms::kWords) {
    if ((w.word >> 26) != 0x25U) continue;  // not a bl
    std::int64_t imm = w.word & 0x3ffffffU;
    if ((imm & (1 << 25)) != 0) imm -= (1 << 26);
    const std::uint64_t target = w.vaddr + static_cast<std::uint64_t>(imm * 4);
    if (target == d.current) ++current;
    else if (target == d.listDataCtor) ++listData;
    else if (target == d.currentPtr) ++currentPtr;
    else if (target == d.memblockCtor) ++memblock;
    else ++other;
  }
  QCHECK(current == 1 && listData == 1 && currentPtr == 11 && memblock == 11 && other == 0);
}

// A pin the process cannot read is a mismatch (a word that is not there is not the pinned build's), and says it could not be read.
void APinWordThatCannotBeReadIsAMismatch() {
  Begin1();
  Build();
  const ts::PinWord pins[] = {{0xf1bcc0, 0xd0013e50}, {0x7fffffff00000000ULL, 0x1}};
  std::uint32_t have = 7;
  bool readable = true;
  const ts::PinWord* const bad = ts::FirstPinMismatch(g_image.Base(), pins, 2, &have, &readable);
  QCHECK(bad == &pins[1] && !readable && have == 0);
  const ts::PinWord wrong[] = {{0xf1bcc0, 0xd0013e51}};
  QCHECK(ts::FirstPinMismatch(g_image.Base(), wrong, 1, &have, &readable) == &wrong[0] && readable && have == 0xd0013e50);
  QCHECK(ts::FirstPinMismatch(g_image.Base(), pins, 1, &have, &readable) == nullptr);
  End1();
}

// In a combat match the object is the reconstruction's, in the stock order, with step 1; the game's constructor is not called.
void ACombatMatchBuildsTheMeshListInTheStockOrderWithStepOne() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  const nevr_quest::combat::ModuleResult r = ms::InstallAt(&Lookup, g_pins, &g_fns, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(Hooked());
  std::uint64_t init[3] = {0, 0x1111, 0x2222};  // CPresenceInit: +8 and +0x10 are copied
  std::uint8_t* obj = Construct(init);
  if (obj != nullptr) {
    QCHECK(g_stockCalls == 0);  // the game's constructor did not run
    QCHECK(Word64(obj, 0x00) == 0x2010);  // the mesh vtable (stored word 0x2000 + 0x10), set last
    QCHECK(Word64(obj, 0x08) == 0 && Word64(obj, 0x10) == 0 && Word64(obj, 0x18) == 0);
    QCHECK(Word64(obj, 0x20) == 0x1111 && Word64(obj, 0x28) == 0x2222);  // copied from init+8 and init+0x10
    QCHECK(Word64(obj, 0x30) == kCurrent && Word64(obj, 0x38) == ~std::uint64_t{0});
    QCHECK(Word64(obj, 0x40) == 0 && Word64(obj, 0x48) == 0 && Word64(obj, 0x50) == 0 && Word64(obj, 0x58) == 0);
    QCHECK(Word64(obj, 0x60) == 7 && Word64(obj, 0x68) == 0);  // the stock q1 constant
    QCHECK(g_vptrAtCurrent == 0x1010);  // the CPresence vtable (stored word 0x1000 + 0x10) was in place when Current() was called, before the mesh vtable
    QCHECK(g_vptrAtListData == 0x2010);  // and the mesh vtable was in place before the list data was constructed
    QCHECK(g_listCalls == 1 && g_listSelf == obj + 0x70);
    // nothing outside the stock constructor's stores is written: the bytes it leaves alone keep the allocation's pattern
    QCHECK(obj[0x78] == 0xA5 && obj[0x2af] == 0xA5 && obj[kObjectBytes - 1] == 0xA5 && obj[kTableOffsets[0] + 0x1f] == 0xEE && obj[kTableOffsets[10] + 0x38] == 0xA5);
    QCHECK(g_blockCalls == 11);
    for (unsigned i = 0; i < 11 && i < g_blockCalls; ++i) {
      QCHECK(g_blockSelf[i] == obj + kTableOffsets[i]);
      QCHECK(g_blockA[i] == 0 && g_blockB[i] == 0 && g_blockAlloc[i] == kCurrentPtr);
      QCHECK(Word64(obj, kTableOffsets[i] + 0x20) == 1);  // stock: 32
      QCHECK(Word64(obj, kTableOffsets[i] + 0x28) == 0 && Word64(obj, kTableOffsets[i] + 0x30) == 0);
    }
    std::free(obj);
  }
  QCHECK(ms::CurrentStats().built == 1 && ms::CurrentStats().stock == 0);
  const std::vector<Line> built = Lines("combat_meshstep_built");
  QCHECK(built.size() == 1U && Lines("combat_meshstep_stock").empty());
  if (built.size() == 1U) {
    QCHECK(built[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(built[0].text) ==
           "\"event\":\"combat_meshstep_built\",\"scope\":\"combat\",\"growth_step\":1,\"stock_growth_step\":32,"
           "\"msg\":\"the first mesh list built in a combat match has the table growth step 1\"}");
  }
  std::uint64_t again[3] = {0, 3, 4};
  std::free(Construct(again));
  QCHECK(ms::CurrentStats().built == 2 && Lines("combat_meshstep_built").size() == 1U);  // the line is written once
  End1();
}

// Arena shares the APK. Everywhere but a combat match the game's constructor runs once and the object is exactly what it made.
void EveryOtherScopeIsTheGamesOwnConstructor() {
  struct Case {
    Where where;
    const char* scope;
  };
  const Case cases[] = {{Where::kArena, "other"},          {Where::kLobbyArenaUses, "other"}, {Where::kCombatLobby, "other"},
                        {Where::kCombatMapDuringArena, "other"}, {Where::kNothingLoaded, "other"}};
  for (const Case& c : cases) {
    Begin1();
    Build();
    SetWhere(c.where);
    QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
    std::uint64_t init[3] = {0, 0x1111, 0x2222};
    const std::uint32_t readsBefore = ts::ScopeReadsForTest();
    std::uint8_t* obj = Construct(init);
    QCHECK(ts::ScopeReadsForTest() - readsBefore == 1U);  // the whole cost on this path: one read of the loaded levels
    if (obj != nullptr) {
      QCHECK(g_stockCalls == 1 && g_stockSelf == obj && g_stockInit == init);  // the game's constructor, once, with the game's arguments
      QCHECK(g_listCalls == 0 && g_blockCalls == 0);                             // none of the reconstruction's helper calls
      for (const std::uintptr_t off : kTableOffsets) QCHECK(Word64(obj, off + 0x20) == 32);  // the stock step
      std::uint8_t* const ref = ReferenceObject(init);
      QCHECK(ref != nullptr && std::memcmp(obj, ref, kObjectBytes) == 0);        // the object is what the game made, byte for byte, all of it
      std::free(ref);
      std::free(obj);
    }
    std::free(Construct(init));  // a second one: counted, and the line is still written once
    QCHECK(ms::CurrentStats().built == 0 && ms::CurrentStats().stock == 2);
    const std::vector<Line> stock = Lines("combat_meshstep_stock");
    QCHECK(stock.size() == 1U && Lines("combat_meshstep_built").empty());
    if (stock.size() == 1U) {
      QCHECK(Has(stock[0].text, std::string("\"scope\":\"") + c.scope + "\"") && stock[0].level == sentinel::LogLevel::kInfo);
    }
    End1();
  }
}

// The resources of a combat map are built while its gamespace is still loading (in the in-flight table only, not yet in the two tables): that is a
// combat match, and an Echo Arena level that is loading is not, whatever else is loaded.
void AMapThatIsStillLoadingIsACombatMatchAndAnArenaLevelThatIsLoadingIsNot() {
  for (const Where where : {Where::kCombatMapLoading, Where::kCombatMapLoadingAfterCombatLobby}) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
    std::uint64_t init[3] = {0, 1, 2};
    std::free(Construct(init));
    QCHECK(g_stockCalls == 0 && g_blockCalls == 11 && ms::CurrentStats().built == 1);
    End1();
  }
  {  // a fiber count the scope does not trust (more than 64) is no level
    Begin1();
    Build();
    SetWhere(Where::kCombatMapLoading);
    g_game.words[kFiberCount / 8] = 65;
    QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
    std::uint64_t init[3] = {0, 1, 2};
    std::free(Construct(init));
    QCHECK(g_stockCalls == 1 && ms::CurrentStats().built == 0);
    End1();
  }
  for (const Where where : {Where::kArenaLoadingAfterCombat, Where::kArenaAndCombatLoading}) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
    std::uint64_t init[3] = {0, 1, 2};
    std::uint8_t* const obj = Construct(init);
    std::uint8_t* const ref = ReferenceObject(init);
    QCHECK(g_stockCalls == 1 && g_blockCalls == 0 && ms::CurrentStats().stock == 1);
    QCHECK(obj != nullptr && ref != nullptr && std::memcmp(obj, ref, kObjectBytes) == 0);
    std::free(ref);
    std::free(obj);
    End1();
  }
}

// Each change of the loaded level writes one counts line per module with what the level that ended cost: the objects left as the game made them
// and the objects changed. Nothing is written for a level that built nothing, and none before the level changes (the arena cost is measurable).
void ALevelChangeWritesTheCountsOfTheLevelThatEnded() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
  std::uint64_t init[3] = {0, 1, 2};
  std::free(Construct(init));
  std::free(Construct(init));
  QCHECK(Lines("combat_meshstep_counts").empty());  // the level has not changed, and before the first game there was nothing to report
  SetWhere(Where::kArena);
  std::free(Construct(init));  // the first of the arena level: the combat level ends before it is counted
  std::vector<Line> counts = Lines("combat_meshstep_counts");
  QCHECK(counts.size() == 1U);
  if (counts.size() == 1U) {
    QCHECK(counts[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(counts[0].text) == CountsText("combat_meshstep_counts", LevelHex("mpl_combat_gauss"), "mpl_combat_gauss", "combat", 0, 2));
  }
  std::free(Construct(init));
  std::free(Construct(init));
  SetWhere(Where::kNothingLoaded);
  std::free(Construct(init));
  counts = Lines("combat_meshstep_counts");
  QCHECK(counts.size() == 2U);
  if (counts.size() == 2U) QCHECK(Golden(counts[1].text) == CountsText("combat_meshstep_counts", LevelHex("mpl_arena_a"), "mpl_arena_a", "other", 3, 0));
  // The counts are since the previous line, not since the start: two more with nothing loaded, then the arena again.
  std::free(Construct(init));
  std::free(Construct(init));
  SetWhere(Where::kArena);
  std::free(Construct(init));
  counts = Lines("combat_meshstep_counts");
  QCHECK(counts.size() == 3U);
  if (counts.size() == 3U) QCHECK(Golden(counts[2].text) == CountsText("combat_meshstep_counts", "0x0000000000000000", "other", "other", 3, 0));
  End1();
}

// What was built before any game was known is reported once the first level is: the scope changes while the level (none) does not.
void WhatWasBuiltBeforeAnyGameWasKnownIsReportedWhenOneIs() {
  Begin1();
  Build();
  ts::SetGameForTest(&NoModelGame);
  QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
  std::uint64_t init[3] = {0, 1, 2};
  std::free(Construct(init));
  std::free(Construct(init));
  QCHECK(Lines("combat_meshstep_counts").empty());
  ts::SetGameForTest(&ModelGame);
  SetWhere(Where::kNothingLoaded);
  std::free(Construct(init));
  const std::vector<Line> counts = Lines("combat_meshstep_counts");
  QCHECK(counts.size() == 1U);
  if (counts.size() == 1U) QCHECK(Golden(counts[0].text) == CountsText("combat_meshstep_counts", "0x0000000000000000", "other", "unknown", 2, 0));
  End1();
}

// A module registered twice keeps one row (a second install, or a test that installs again), and a row without an event or a function is refused.
std::uint32_t g_fakeLeft = 0;
ts::Counts FakeCounts() noexcept { return {g_fakeLeft, 0}; }
void ARegistrationIsKeptOnce() {
  Begin1();
  ts::SetGameForTest(&ModelGame);
  SetWhere(Where::kArena);
  ts::RegisterCounts("combat_fake_counts", &FakeCounts);
  ts::RegisterCounts("combat_fake_counts", &FakeCounts);
  ts::RegisterCounts(nullptr, &FakeCounts);
  ts::RegisterCounts("combat_other_counts", nullptr);
  g_fakeLeft = 0;
  (void)ts::CurrentScope();  // the arena level becomes the last one seen
  g_fakeLeft = 4;
  SetWhere(Where::kCombatMap);
  (void)ts::CurrentScope();  // and ends
  QCHECK(Lines("combat_fake_counts").size() == 1U && Lines("combat_other_counts").empty());
  g_fakeLeft = 0;
  End1();
}

// No game known yet (the combat tick has not verified one): the game's constructor, and no read of any level.
void WithNoGameKnownTheGameBuildsItsOwnObject() {
  Begin1();
  Build();
  ts::SetGameForTest(&NoModelGame);
  QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
  std::uint64_t init[3] = {0, 1, 2};
  std::uint8_t* obj = Construct(init);
  QCHECK(ts::ScopeReadsForTest() == 0U && g_stockCalls == 1 && g_blockCalls == 0);
  std::uint8_t* const ref = ReferenceObject(init);
  QCHECK(obj != nullptr && ref != nullptr && std::memcmp(obj, ref, kObjectBytes) == 0);  // byte for byte the game's, with no game known too
  std::free(ref);
  std::free(obj);
  const std::vector<Line> stock = Lines("combat_meshstep_stock");
  QCHECK(stock.size() == 1U && Has(stock[0].text, "\"scope\":\"unknown\""));
  End1();
}

// A null object or a null init is the game's to handle: the handler does not reconstruct for them.
void ANullObjectOrInitGoesToTheGame() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
  std::uint64_t init[3] = {};
  SlotFn()(nullptr, init);
  QCHECK(g_stockCalls == 1 && g_blockCalls == 0 && g_stockSelf == nullptr);
  auto* obj = static_cast<std::uint8_t*>(std::calloc(1, kObjectBytes));
  SlotFn()(obj, nullptr);
  QCHECK(g_stockCalls == 2 && g_blockCalls == 0 && g_stockInit == nullptr);
  std::free(obj);
  End1();
}

void AnInstallRefusalLeavesTheSlotAndTheConstructorAlone() {
  struct Refusal {
    const char* name;
    const char* reason;
  };
  {  // a pinned word that differs (the PLT stub's adrp): refused whole, nothing hooked, the Warning names the word
    Begin1();
    Build();
    g_image.Put<std::uint32_t>(0xf1bcc0, 0x52800034);
    const nevr_quest::combat::ModuleResult r = ms::InstallAt(&Lookup, g_pins, &g_fns, true);
    QCHECK(!r.installed && std::strcmp(r.status, "pinned_word_mismatch") == 0);
    QCHECK(!Hooked() && *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&FakeStockCtor));
    const std::vector<Line> pin = Lines("combat_meshstep_pin");
    QCHECK(pin.size() == 1U && pin[0].level == sentinel::LogLevel::kWarn);
    if (pin.size() == 1U) QCHECK(Has(pin[0].text, "\"vaddr\":\"0x0000000000f1bcc0\"") && Has(pin[0].text, "\"have\":\"0x0000000052800034\""));
    const std::vector<Line> install = Lines("combat_meshstep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"pinned_word_mismatch\""));
    End1();
  }
  {  // the last pinned word, a bl of the last helper call
    Begin1();
    Build();
    g_image.Put<std::uint32_t>(0x18679b4, 0);
    QCHECK(std::strcmp(ms::InstallAt(&Lookup, g_pins, &g_fns, true).status, "pinned_word_mismatch") == 0 && !Hooked());
    End1();
  }
  {  // a zero vtable word
    Begin1();
    Build();
    g_image.Put<std::uintptr_t>(g_pins.vtblMesh, 0);
    QCHECK(std::strcmp(ms::InstallAt(&Lookup, g_pins, &g_fns, true).status, "vtable_unreadable") == 0 && !Hooked());
    End1();
  }
  {  // a missing helper
    Begin1();
    Build();
    ms::Fns missing = FakeFns();
    missing.memblockCtor = nullptr;
    QCHECK(std::strcmp(ms::InstallAt(&Lookup, g_pins, &missing, true).status, "helper_missing") == 0 && !Hooked());
    End1();
  }
  {  // a slot that holds another function (another hook, or another build): the manager refuses it and leaves it
    Begin1();
    Build(/*foreignSlot=*/true);
    const nevr_quest::combat::ModuleResult r = ms::InstallAt(&Lookup, g_pins, &g_fns, true);
    QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
    QCHECK(!Hooked() && *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&ForeignCtor));
    const std::vector<Line> install = Lines("combat_meshstep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
    End1();
  }
  {  // the build is not the pinned one; libr15 is not loaded
    Begin1();
    g_pins = ms::kPins;
    g_image.Build({{ms::kSymbol, g_pins.slot, false, false, g_pins.ctor}}, "0000000000000000000000000000000000000000");
    QCHECK(std::strcmp(ms::InstallAt(&Lookup, g_pins, &g_fns, true).status, "build_id_mismatch") == 0);
    g_image.Drop();
    QCHECK(std::strcmp(ms::InstallAt(&Lookup, g_pins, &g_fns, true).status, "already_ran") == 0);  // the first refusal used the step
    End1();
    Begin1();
    QCHECK(std::strcmp(ms::InstallAt(&Lookup, g_pins, &g_fns, true).status, "module_not_loaded") == 0);
    End1();
  }
}

void TheSwitchAndASecondStepInstallNothing() {
  {  // off: nothing hooked, Info with the key's reason, the module is reported not installed
    Begin1();
    Build();
    const nevr_quest::combat::ModuleResult r = ms::InstallAt(&Lookup, g_pins, &g_fns, false);
    QCHECK(!r.installed && std::strcmp(r.status, "off") == 0 && !Hooked());
    const std::vector<Line> install = Lines("combat_meshstep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_meshstep_install\",\"result\":\"off\",\"reason\":\"off_config\",\"status\":\"off\","
             "\"slot_vaddr\":\"0x00000000036e5548\","
             "\"consequence\":\"combat.meshstep is false: mesh lists are built by the game's constructor, growth step 32\"}");
    }
    End1();
  }
  {  // installed once; the second step is refused and changes nothing
    Begin1();
    Build();
    SetWhere(Where::kCombatMap);
    QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed);
    const std::vector<Line> install = Lines("combat_meshstep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_meshstep_install\",\"result\":\"installed\",\"reason\":\"ok\",\"status\":\"installed\","
             "\"slot_vaddr\":\"0x00000000036e5548\","
             "\"consequence\":\"mesh lists built in a combat match have the table growth step 1; arena keeps the game's constructor\"}");
    }
    const nevr_quest::combat::ModuleResult again = ms::InstallAt(&Lookup, g_pins, &g_fns, true);
    QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0 && Hooked());
    End1();
  }
}

// The plugin's kill file is not read; one found in its data directory is a Warning that names the key that replaces it, and the module installs.
void TheKillFileIsOnlyAWarning() {
  Begin1();
  Build();
  const std::string dir = MakeLegacyDir("meshstep.off");
  ts::SetLegacyDirForTest(dir.c_str());
  QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed && Hooked());  // the file turned nothing off
  const std::vector<Line> w = Lines("combat_meshstep_legacy_switch");
  QCHECK(w.size() == 1U);
  if (w.size() == 1U) {
    QCHECK(w[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(w[0].text, "\"file\":\"meshstep.off\"") && Has(w[0].text, "\"replaced_by\":\"combat.meshstep: false\""));
  }
  RemoveLegacyDir(dir, "meshstep.off");
  End1();
  Begin1();  // no file, no line
  Build();
  QCHECK(ms::InstallAt(&Lookup, g_pins, &g_fns, true).installed && Lines("combat_meshstep_legacy_switch").empty());
  End1();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  ThePinnedWordsNameTheSlotTheVtablesAndTheFourHelpers();
  APinWordThatCannotBeReadIsAMismatch();
  ACombatMatchBuildsTheMeshListInTheStockOrderWithStepOne();
  EveryOtherScopeIsTheGamesOwnConstructor();
  AMapThatIsStillLoadingIsACombatMatchAndAnArenaLevelThatIsLoadingIsNot();
  ALevelChangeWritesTheCountsOfTheLevelThatEnded();
  WhatWasBuiltBeforeAnyGameWasKnownIsReportedWhenOneIs();
  ARegistrationIsKeptOnce();
  WithNoGameKnownTheGameBuildsItsOwnObject();
  ANullObjectOrInitGoesToTheGame();
  AnInstallRefusalLeavesTheSlotAndTheConstructorAlone();
  TheSwitchAndASecondStepInstallNothing();
  TheKillFileIsOnlyAWarning();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "meshstep_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("meshstep_test: all checks passed");
  return 0;
}
