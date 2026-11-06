// Host test of the two factory words together (src/quest/combat/shaderstep.cpp and tablestep.cpp, #2177): ConstructInstance (vtable slot 1,
// ABS64 0x365ddc8) and CreateInstance (slot 2, 0x365ddd0) are neighbouring words of one factory vtable, and both modules hook one of them as a
// class C word of docs/adr/0046. In one libr15 image built in memory with both relocations: each module installs on its own word whichever goes
// first, a call through each word gets the step 1 in a combat match and the game's object elsewhere, the neighbouring words of the vtable are not
// touched, and a module the manager refuses leaves the other one running (one owner per word).
//
// Built and run by `just test-quest-combat`.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/shaderstep.h"
#include "quest/combat/tablestep.h"
#include "quest/tests/table_step_fixture.h"

namespace {

namespace ss = nevr_quest::combat::shaderstep;
namespace tbs = nevr_quest::combat::tablestep;
using namespace table_step_test;

constexpr std::size_t kStepOff = 0x98;
constexpr std::size_t kObjBytes = 0xb0;

std::uint64_t g_step = 0x20;
extern "C" __attribute__((noinline)) void* FakeConstruct(void*, void* obj, const void*) {
  std::memset(obj, 0xab, kObjBytes);
  std::memcpy(static_cast<std::uint8_t*>(obj) + kStepOff, &g_step, sizeof g_step);
  return obj;
}
extern "C" __attribute__((noinline)) void* FakeCreate(void*, void*) {
  auto* obj = static_cast<std::uint8_t*>(std::calloc(1, kObjBytes));
  std::memcpy(obj + kStepOff, &g_step, sizeof g_step);
  return obj;
}
extern "C" __attribute__((noinline)) void* ForeignConstruct(void*, void* obj, const void*) { return obj; }
using ConstructFn = void* (*)(void*, void*, const void*);
using CreateFn = void* (*)(void*, void*);

ss::Pins g_ss = ss::kPins;
tbs::Pins g_tbs = tbs::kPins;

std::uint64_t Step(const void* obj) {
  std::uint64_t v = 0;
  std::memcpy(&v, static_cast<const std::uint8_t*>(obj) + kStepOff, sizeof v);
  return v;
}

constexpr std::uint64_t kNeighbourBefore = 0x365ddc0, kNeighbourAfter = 0x365ddd8;  // the factory's destructor word and CreateData's

void Build() {
  g_ss = ss::kPins;
  g_tbs = tbs::kPins;
  g_image.Build({{ss::kSymbol, g_ss.slot, false, false, g_ss.fn, true}, {tbs::kSymbol, g_tbs.slot, false, false, g_tbs.fn, true}});
  g_ss.fn = reinterpret_cast<std::uintptr_t>(&FakeConstruct) - g_image.Base();
  g_tbs.fn = reinterpret_cast<std::uintptr_t>(&FakeCreate) - g_image.Base();
  PutWords(tbs::kWords);
  *g_image.Slot(g_ss.slot) = reinterpret_cast<void*>(&FakeConstruct);
  *g_image.Slot(g_tbs.slot) = reinterpret_cast<void*>(&FakeCreate);
  *g_image.Slot(kNeighbourBefore) = reinterpret_cast<void*>(0x111110);
  *g_image.Slot(kNeighbourAfter) = reinterpret_cast<void*>(0x222220);
}
void Begin1() {
  Begin();
  ss::ResetForTest();
  tbs::ResetForTest();
  ss::SetImageLookupForTest(&Lookup);
  tbs::SetImageLookupForTest(&Lookup);
  ss::SetPinsForTest(&g_ss);
  tbs::SetPinsForTest(&g_tbs);
  g_step = 0x20;
}
void End1() {
  ss::ResetForTest();
  tbs::ResetForTest();
  ss::SetImageLookupForTest(nullptr);
  tbs::SetImageLookupForTest(nullptr);
  ss::SetPinsForTest(nullptr);
  tbs::SetPinsForTest(nullptr);
  End();
}
bool SsHooked() { return *g_image.Slot(g_ss.slot) == ss::Thunk::EntryAddress(); }
bool TbsHooked() { return *g_image.Slot(g_tbs.slot) == tbs::Thunk::EntryAddress(); }
ConstructFn SsFn() { return reinterpret_cast<ConstructFn>(*g_image.Slot(g_ss.slot)); }
CreateFn TbsFn() { return reinterpret_cast<CreateFn>(*g_image.Slot(g_tbs.slot)); }

void EachModuleOwnsItsOwnWordInEitherInstallOrder() {
  for (const bool shaderFirst : {true, false}) {
    Begin1();
    Build();
    SetWhere(Where::kCombatMap);
    if (shaderFirst) {
      QCHECK(ss::InstallAt(&Lookup, g_ss, true).installed);
      QCHECK(tbs::InstallAt(&Lookup, g_tbs, true).installed);
    } else {
      QCHECK(tbs::InstallAt(&Lookup, g_tbs, true).installed);
      QCHECK(ss::InstallAt(&Lookup, g_ss, true).installed);
    }
    QCHECK(SsHooked() && TbsHooked());
    QCHECK(ss::Thunk::EntryAddress() != tbs::Thunk::EntryAddress());
    QCHECK(*g_image.Slot(kNeighbourBefore) == reinterpret_cast<void*>(0x111110));  // the vtable's other words are the game's
    QCHECK(*g_image.Slot(kNeighbourAfter) == reinterpret_cast<void*>(0x222220));
    alignas(8) std::uint8_t built[kObjBytes] = {};
    QCHECK(SsFn()(nullptr, built, nullptr) == built && Step(built) == 1);
    void* const created = TbsFn()(nullptr, nullptr);
    QCHECK(created != nullptr && Step(created) == 1);
    std::free(created);
    QCHECK(ss::CurrentStats().stepped == 1 && tbs::CurrentStats().rewritten == 1);  // each handler counted its own call only
    End1();
  }
}

void InArenaBothWordsPassTheGamesObjectThrough() {
  Begin1();
  Build();
  SetWhere(Where::kArena);
  QCHECK(ss::InstallAt(&Lookup, g_ss, true).installed && tbs::InstallAt(&Lookup, g_tbs, true).installed);
  alignas(8) std::uint8_t built[kObjBytes] = {};
  SsFn()(nullptr, built, nullptr);
  QCHECK(Step(built) == 0x20);
  void* const created = TbsFn()(nullptr, nullptr);
  QCHECK(created != nullptr && Step(created) == 0x20);
  std::free(created);
  QCHECK(ss::CurrentStats().stock == 1 && tbs::CurrentStats().stock == 1);
  End1();
}

// A word the manager refuses (a foreign hook's) is that module's alone: the other module still installs and runs.
void ARefusedWordDoesNotTakeTheOtherModuleDown() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  *g_image.Slot(g_ss.slot) = reinterpret_cast<void*>(&ForeignConstruct);
  QCHECK(!ss::InstallAt(&Lookup, g_ss, true).installed && !SsHooked());
  QCHECK(tbs::InstallAt(&Lookup, g_tbs, true).installed && TbsHooked());
  void* const created = TbsFn()(nullptr, nullptr);
  QCHECK(created != nullptr && Step(created) == 1);
  std::free(created);
  QCHECK(*g_image.Slot(g_ss.slot) == reinterpret_cast<void*>(&ForeignConstruct));
  End1();
}

}  // namespace

int main() {
  EachModuleOwnsItsOwnWordInEitherInstallOrder();
  InArenaBothWordsPassTheGamesObjectThrough();
  ARefusedWordDoesNotTakeTheOtherModuleDown();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "shaderstep_tablestep_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("shaderstep_tablestep_test: all checks passed");
  return 0;
}
