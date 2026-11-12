// Host test of droneguard (src/quest/combat/droneguard.cpp, #2178): the real handler through the thunk, installed by the real GotHook into the
// JUMP_SLOT of a libr15 image built in memory, and called the way the game calls it: through the slot. The game side is a model: a fake
// CR15NetFollowPlayerCS (target at +0x140, the player handle's cache at +0x148 / +0x16c / +0x188 / +0x190), a stand-in for Follow, and fake
// RefreshCache / IsComponentValid.
//
// What it pins: a set target reaches the game's Follow with the same arguments, a null
// target never does (one first-skip line, a total every 600th skip at most 8 times, a target-set line when it arrives), the state line on a change of the
// cache (at most 16, read-only), an unreadable system goes to Follow, stage 1 refreshes first and then skips on idx ffff, handle-sys 0 or a refused index
// (reading the cache the refresh just wrote), the missing or misplaced engine functions and a changed stage-1 word leave the null half only with one Error
// line, a slot that does not hold Follow is refused and left alone, every stage-0 word flipped in turn (and an unreadable one) refuses the install. New with
// the move: the switch, stage 1 never tried when the guard is off, the statuses, the stats lines, the missing image and build ID, and that the guard acts in
// every level (it has no level scope).
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/droneguard.h"

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pin_check.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/test_check.h"

namespace {

namespace dg = nevr_quest::combat::droneguard;
namespace pinned = sentinel::pinned;
using combat_test::Has;
using combat_test::Image;
using combat_test::Line;
using combat_test::Lines;
using nevr_quest::combat::ModuleResult;
using nevr_quest::combat::pinned::Word;
// The tables themselves, not the module's own `kWordPins` (which a bug could shorten): the test puts every pinned word in the image and flips every one.
const nevr_quest::combat::pinned::Pins kAllWords = {dg::kWords, dg::kWordCount, nullptr, 0};
const nevr_quest::combat::pinned::Pins kAllIdxWords = {dg::kIdxWords, dg::kIdxWordCount, nullptr, 0};

using FollowFn = void (*)(void*, std::uint64_t);

// ---- the fake engine ------------------------------------------------------------------------------------------------------------

alignas(16) std::uint8_t g_cs[0x200];
alignas(16) std::uint8_t g_cs2[0x200];  // a second follow component
void Put64(std::size_t off, std::uint64_t v) { std::memcpy(g_cs + off, &v, 8); }
void Put32(std::size_t off, std::uint32_t v) { std::memcpy(g_cs + off, &v, 4); }

constexpr std::uintptr_t kSys = 0x5000, kTarget = 0x6000;
int g_realCalls = 0;
void* g_realCs = nullptr;
std::uint64_t g_realHandle = 0;
extern "C" __attribute__((noinline)) void RealFollow(void* cs, std::uint64_t handle) {
  ++g_realCalls;
  g_realCs = cs;
  g_realHandle = handle;
}
extern "C" __attribute__((noinline)) void OtherFollow(void*, std::uint64_t) {}

// RefreshCache fake: records the handle it got and, like the engine, rewrites the cache (sys / idx) from the test's wish.
int g_refreshCalls = 0;
void* g_refreshArg = nullptr;
std::uintptr_t g_afterSys = kSys;
std::uint32_t g_afterIdx = 0x00070003;
int g_validCalls = 0;
const void* g_validSys = nullptr;
std::uint64_t g_validIdx = 0;
bool g_valid = true;
void* g_poison = nullptr;  // when set, the refresh makes this page unreadable instead of rewriting the cache
void FRefresh(void* handle) {
  ++g_refreshCalls;
  g_refreshArg = handle;
  if (g_poison != nullptr) {
    mprotect(g_poison, 4096, PROT_NONE);
    return;
  }
  Put64(0x188, g_afterSys);
  Put32(0x190, g_afterIdx);
}
bool FValid(const void* sys, std::uint64_t idx) {
  ++g_validCalls;
  g_validSys = sys;
  g_validIdx = idx;
  return g_valid;
}
dg::EngineFns Fns() {
  dg::EngineFns f{};
  f.refresh = &FRefresh;
  f.isValid = &FValid;
  return f;
}

void World(std::uintptr_t target) {
  std::memset(g_cs, 0, sizeof(g_cs));
  Put64(0x140, target);
  Put64(0x188, kSys);
  Put32(0x190, 0x00070003);
  g_cs[0x16c] = 0;
  g_realCalls = g_refreshCalls = g_validCalls = 0;
  g_afterSys = kSys;
  g_afterIdx = 0x00070003;
  g_valid = true;
}

// ---- the libr15 image ----------------------------------------------------------------------------------------------------------

Image g_image;
dg::Pins g_pins = dg::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// A libr15 holding the pinned words (both stages) and the slot, which holds `slotWord` (the stand-in when 0).
void Build(std::uintptr_t slotWord = 0) {
  g_pins = dg::kPins;
  g_image.Build({{dg::kFollowSymbol, g_pins.slot, false, false, g_pins.fn}});
  // base + fn is the stand-in's address: the real function is not in the image, so the pin holds the stand-in's offset.
  g_pins.fn = reinterpret_cast<std::uintptr_t>(&RealFollow) - g_image.Base();
  combat_test::PutPins(g_image, &kAllWords, 1);
  combat_test::PutPins(g_image, &kAllIdxWords, 1);
  *g_image.Slot(g_pins.slot) = slotWord != 0 ? reinterpret_cast<void*>(slotWord) : reinterpret_cast<void*>(&RealFollow);
}
FollowFn Slot() { return reinterpret_cast<FollowFn>(*g_image.Slot(g_pins.slot)); }
bool Hooked() { return *g_image.Slot(g_pins.slot) == dg::FollowThunk::EntryAddress(); }
bool SlotIsTheGames() { return *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&RealFollow); }

void Begin() {
  combat_test::StartCapture();
  dg::ResetForTest();
  Build();
}
void End() {
  dg::ResetForTest();
  g_image.Drop();
  combat_test::StopCapture();
}

ModuleResult Install(bool on, bool stage1, const dg::EngineFns* fns) { return dg::InstallAt(&Lookup, g_pins, on, stage1, fns); }

// A fresh world with the guard installed (stage 1 as asked, with the fake engine functions); returns the hooked entry.
FollowFn Installed(bool stage1) {
  End();
  Begin();
  const dg::EngineFns f = Fns();
  const ModuleResult r = Install(true, stage1, &f);
  QCHECK(r.installed && Hooked());
  return Slot();
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(dg::kFollowSlotVaddr == 0x36f1218 && dg::kFollowVaddr == 0x2149764 && dg::kRefreshCacheVaddr == 0x1210848 && dg::kIsValidVaddr == 0x1cdb3e0);
  QCHECK(std::strcmp(dg::kFollowSymbol, "_ZN10NRadEngine8NRadGame21CR15NetFollowPlayerCS6FollowENS_18SCompactPoolHandleE") == 0);
  QCHECK(std::strcmp(dg::kRefreshCacheSymbol, "_ZNK10NRadEngine16CComponentHandleINS_8NRadGame15CR15PlayerNavCSEE12RefreshCacheEv") == 0);
  QCHECK(std::strcmp(dg::kIsValidSymbol, "_ZNK10NRadEngine8NRadGame15CR15PlayerNavCS16IsComponentValidENS_18SCompactPoolHandleE") == 0);
  QCHECK(dg::kOffTarget == 0x140 && dg::kOffHandle == 0x148 && dg::kOffHandleFlags == 0x16c && dg::kOffHandleSys == 0x188 && dg::kOffHandleIdx == 0x190);
  static const Word kWant[4] = {{"", 0x21497bc, 0xf940a274}, {"", 0x2149744, 0x97b7a7c7}, {"", 0x21497cc, 0xb9419261}, {"", 0x214980c, 0x79432268}};
  QCHECK(dg::kWordCount == 4);
  for (std::size_t i = 0; i < 4; ++i) QCHECK(dg::kWords[i].vaddr == kWant[i].vaddr && dg::kWords[i].word == kWant[i].word && dg::kWords[i].what[0] != '\0');
  QCHECK(combat_test::Fingerprint(dg::kWords, dg::kWordCount) == combat_test::Fingerprint(kWant, 4));
  static const Word kWantIdx[3] = {{"", 0x21497c8, 0x97b70b0a}, {"", 0x1cdb3e0, 0x7941e008}, {"", 0x1cdb3f8, 0x6b41413f}};
  QCHECK(dg::kIdxWordCount == 3);
  for (std::size_t i = 0; i < 3; ++i) QCHECK(dg::kIdxWords[i].vaddr == kWantIdx[i].vaddr && dg::kIdxWords[i].word == kWantIdx[i].word && dg::kIdxWords[i].what[0] != '\0');
  QCHECK(combat_test::Fingerprint(dg::kIdxWords, dg::kIdxWordCount) == combat_test::Fingerprint(kWantIdx, 3));
}

// ---- stage 0 ----

void TheThunkTakesTheSlotAndTheLineSaysSo() {
  Begin();
  const ModuleResult r = Install(true, false, nullptr);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off && Hooked());
  const std::vector<Line> install = Lines("combat_droneguard_install");
  QCHECK(install.size() == 1U && Lines("combat_droneguard_stage1").empty());
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_droneguard_install\",\"result\":\"installed\",\"reason\":\"ok\",\"hook\":\"ok\",\"stage1\":\"off\",\"droneguard_stage1\":0,"
           "\"consequence\":\"the drone's follow update is skipped while its target is null instead of crashing the game, in every level\"}");
  }
  End();
}

// 1. A target that is set: the game's function with the same arguments, nothing skipped, stage 1 never touched.
void ASetTargetReachesTheGamesFollowUntouched() {
  const FollowFn hook = Installed(false);
  World(kTarget);
  hook(g_cs, 0xabcd1234ULL);
  QCHECK(g_realCalls == 1 && g_realCs == g_cs && g_realHandle == 0xabcd1234ULL);
  QCHECK(g_refreshCalls == 0 && g_validCalls == 0);
  QCHECK(dg::CurrentStats().skips == 0 && Lines("combat_droneguard_skip").empty());
  End();
}

// 2. A null target: Follow is not called; one first-skip line; a total at every 600th skip, at most 8.
void ANullTargetIsNeverFollowedAndIsCounted() {
  const FollowFn hook = Installed(false);
  World(0);
  hook(g_cs, 1);
  QCHECK(g_realCalls == 0 && dg::CurrentStats().skips == 1);
  const std::vector<Line> first = Lines("combat_droneguard_skip");
  QCHECK(first.size() == 1U);
  if (first.size() == 1U) QCHECK(first[0].level == sentinel::LogLevel::kInfo && Has(first[0].text, "\"cs\":\"0x"));
  for (int i = 0; i < 598; ++i) hook(g_cs, 1);
  QCHECK(Lines("combat_droneguard_skips").empty() && Lines("combat_droneguard_skip").size() == 1U);
  hook(g_cs, 1);
  const std::vector<Line> totals = Lines("combat_droneguard_skips");
  QCHECK(totals.size() == 1U && Has(totals[0].text, "\"skips\":600}"));
  for (int i = 0; i < 600 * 9; ++i) hook(g_cs, 1);
  QCHECK(Lines("combat_droneguard_skips").size() == 8U);
  QCHECK(Lines("combat_droneguard_log_cap").size() == 1U && Has(Lines("combat_droneguard_log_cap")[0].text, "\"lines\":\"skips\",\"cap\":8"));  // the 9th total said they stopped
  QCHECK(g_realCalls == 0);
  End();
}

// 3. The target arrives: one target-set line with the skip count, Follow runs; a later null starts a new first-skip line.
void TheTargetArrivingEndsTheRunOfSkips() {
  const FollowFn hook = Installed(false);
  World(0);
  for (int i = 0; i < 3; ++i) hook(g_cs, 1);
  combat_test::Captured().clear();
  const std::uint64_t skips = dg::CurrentStats().skips;
  QCHECK(skips == 3);
  Put64(0x140, kTarget);
  hook(g_cs, 5);
  hook(g_cs, 6);
  QCHECK(g_realCalls == 2 && g_realHandle == 6);
  const std::vector<Line> set = Lines("combat_droneguard_target_set");
  QCHECK(set.size() == 1U && set[0].level == sentinel::LogLevel::kInfo && Has(set[0].text, "\"skips\":3"));
  Put64(0x140, 0);
  hook(g_cs, 7);
  QCHECK(Lines("combat_droneguard_skip").size() == 1U);
  End();
}

// The run of skips belongs to the component: a drone with no target skipped every frame beside a drone whose target is set, both called every frame, is one
// skip line and no target-set line (not two lines every frame), and the same for stage 1's idx pair.
void TwoComponentsEachCalledEveryFrameWriteOneSkipLineAndNoTargetSetLine() {
  const FollowFn hook = Installed(false);
  World(kTarget);
  std::memcpy(g_cs2, g_cs, sizeof(g_cs));  // the second component: target set
  World(0);                                // the first: no target
  for (int frame = 0; frame < 1000; ++frame) {
    hook(g_cs, 1);
    hook(g_cs2, 2);
  }
  QCHECK(g_realCalls == 1000 && dg::CurrentStats().skips == 1000);
  QCHECK(Lines("combat_droneguard_skip").size() == 1U && Lines("combat_droneguard_target_set").empty());
  End();
  const FollowFn staged = Installed(true);
  World(kTarget);
  std::memcpy(g_cs2, g_cs, sizeof(g_cs));
  for (int frame = 0; frame < 1000; ++frame) {
    g_afterIdx = 0x0007ffff;  // the refresh leaves the first component's index unresolved
    staged(g_cs, 1);
    g_afterIdx = 0x00070003;  // and the second's resolved
    staged(g_cs2, 2);
  }
  QCHECK(dg::CurrentStats().idxSkips == 1000);
  QCHECK(Lines("combat_droneguard_idx_skip").size() == 1U && Lines("combat_droneguard_idx_resolved").empty());
  End();
}

// Two components with no target, each skipped every frame, each start a run of their own: two skip lines, not one.
void TwoComponentsWithNoTargetEachWriteTheirOwnSkipLine() {
  const FollowFn hook = Installed(false);
  World(0);
  std::memcpy(g_cs2, g_cs, sizeof(g_cs));
  for (int frame = 0; frame < 100; ++frame) {
    hook(g_cs, 1);
    hook(g_cs2, 2);
  }
  QCHECK(Lines("combat_droneguard_skip").size() == 2U && dg::CurrentStats().skips == 200 && g_realCalls == 0);
  End();
}

// A component that flaps between a null target and a set one every frame cannot write a line a frame: each kind of run line has a cap, the first attempt over it
// writes one Warning that says so, and the counts go on in the stats line.
void AFlappingComponentIsCappedAndTheCapSaysSo() {
  const FollowFn hook = Installed(false);
  World(0);
  for (int frame = 0; frame < 1000; ++frame) {
    Put64(0x140, frame % 2 == 0 ? 0 : kTarget);
    hook(g_cs, 1);
  }
  QCHECK(Lines("combat_droneguard_skip").size() == 8U && Lines("combat_droneguard_target_set").size() == 8U);
  const std::vector<Line> cap = Lines("combat_droneguard_log_cap");
  int skipCap = 0, setCap = 0, stateCap = 0;
  for (const Line& l : cap) {
    QCHECK(l.level == sentinel::LogLevel::kWarn && Has(l.text, "not written"));
    skipCap += Has(l.text, "\"lines\":\"skip\",\"cap\":8") ? 1 : 0;
    setCap += Has(l.text, "\"lines\":\"target_set\",\"cap\":8") ? 1 : 0;
    stateCap += Has(l.text, "\"lines\":\"state\",\"cap\":16") ? 1 : 0;
  }
  QCHECK(cap.size() == 3U && skipCap == 1 && setCap == 1 && stateCap == 1);  // each kind says once that it stopped (the state line flaps with the target too)
  QCHECK(dg::CurrentStats().skips == 500);  // every skip is still counted
  End();
}

// ---- the table of runs (review: arena cost, slots that are never freed) ----

std::int64_t g_nowMs = 100000;
std::int64_t FakeNowMs() noexcept { return g_nowMs; }

// With no run open the common path (a set target) scans nothing: the table is asked only while a run is open. Counted, not timed.
void WithNoRunOpenACallWithATargetScansNoTable() {
  const FollowFn hook = Installed(false);
  World(kTarget);
  for (int i = 0; i < 1000; ++i) hook(g_cs, 1);
  QCHECK(g_realCalls == 1000 && dg::RunScansForTest() == 0);
  World(0);
  hook(g_cs, 1);  // a run opens: the next call with a target has a table to ask
  Put64(0x140, kTarget);
  hook(g_cs, 1);
  QCHECK(dg::RunScansForTest() == 1);
  for (int i = 0; i < 1000; ++i) hook(g_cs, 1);  // and the run is closed: no scan again
  QCHECK(dg::RunScansForTest() == 1);
  End();
}

// There is no signal that a follow component died, so a run that began more than kRunStaleSeconds ago is stale: its target-set line is dropped and counted, not
// written for skips a component at a reused address never had; the slot is reclaimed by a later run.
void ARunOfAComponentThatDiedIsStaleAndItsTargetSetLineIsNotWritten() {
  const FollowFn hook = Installed(false);
  dg::SetClockForTest(&FakeNowMs);
  g_nowMs = 100000;
  World(0);
  hook(g_cs, 1);  // component A: a run begins, then it dies (never called again)
  QCHECK(Lines("combat_droneguard_skip").size() == 1U);
  g_nowMs += (dg::kRunStaleSeconds + 1) * 1000;
  Put64(0x140, kTarget);  // a new component at the same address, with a target
  hook(g_cs, 1);
  QCHECK(Lines("combat_droneguard_target_set").empty() && g_realCalls == 1);
  QCHECK(dg::CurrentStats().runsStale == 1 && dg::CurrentStats().targetSets == 0);
  // a run inside the window ends with its line
  World(0);
  g_nowMs = 100000;
  Put64(0x140, 0);
  hook(g_cs, 1);
  g_nowMs += (dg::kRunStaleSeconds - 1) * 1000;
  Put64(0x140, kTarget);
  hook(g_cs, 1);
  QCHECK(Lines("combat_droneguard_target_set").size() == 1U && dg::CurrentStats().targetSets == 1 && dg::CurrentStats().runsStale == 1);
  End();
}

// Components other than g_cs, each with its own bytes.
alignas(16) std::uint8_t g_many[dg::kRunSlots + 1][0x200];
// One component dies mid-run (its run never ends). The early-out must not be off for good: calls with a target scan while that run is open, the scan releases it once
// it is stale, and after that they scan nothing again.
void ADeadRunIsReleasedByTheScanAndTheEarlyOutComesBack() {
  const FollowFn hook = Installed(false);
  dg::SetClockForTest(&FakeNowMs);
  g_nowMs = 100000;
  World(kTarget);
  std::memcpy(g_many[0], g_cs, sizeof(g_cs));  // the live component, always with a target
  World(0);
  hook(g_cs, 1);  // component A begins a run and dies: it is never called again
  for (int i = 0; i < 10; ++i) hook(g_many[0], 1);
  QCHECK(dg::RunScansForTest() == 10 && dg::CurrentStats().runsStale == 0);  // inside the window the open run is scanned for
  g_nowMs += (dg::kRunStaleSeconds + 1) * 1000;
  for (int i = 0; i < 32; ++i) hook(g_many[0], 1);  // a sweep is every 32nd scan: one of these finds the dead run stale and releases it
  const unsigned drained = dg::RunScansForTest();
  QCHECK(drained > 10 && drained <= 42 && dg::CurrentStats().runsStale == 1 && dg::CurrentStats().targetSets == 0);
  for (int i = 0; i < 1000; ++i) hook(g_many[0], 1);
  QCHECK(dg::RunScansForTest() == drained && Lines("combat_droneguard_target_set").empty());
  End();
}

// A full table says so once and counts; a slot whose run is stale is reclaimed by the next run, so the table cannot fill up for good.
void AFullTableSaysSoAndAStaleSlotIsReclaimed() {
  const FollowFn hook = Installed(false);
  dg::SetClockForTest(&FakeNowMs);
  g_nowMs = 100000;
  World(0);
  for (auto& comp : g_many) std::memcpy(comp, g_cs, sizeof(g_cs));
  for (unsigned i = 0; i < dg::kRunSlots; ++i) hook(g_many[i], 1);
  QCHECK(dg::CurrentStats().runs == dg::kRunSlots && dg::CurrentStats().runTableFull == 0 && Lines("combat_droneguard_run_table_full").empty());
  hook(g_many[dg::kRunSlots], 1);  // the 17th component: no slot
  hook(g_many[dg::kRunSlots], 1);
  QCHECK(dg::CurrentStats().runs == dg::kRunSlots && dg::CurrentStats().runTableFull == 2);
  const std::vector<Line> full = Lines("combat_droneguard_run_table_full");
  QCHECK(full.size() == 1U && full[0].level == sentinel::LogLevel::kWarn && Has(full[0].text, "\"slots\":16") && Has(full[0].text, "counted"));
  g_nowMs += (dg::kRunStaleSeconds + 1) * 1000;
  hook(g_many[dg::kRunSlots], 1);  // the other slots are stale now: this run takes one
  QCHECK(dg::CurrentStats().runs == dg::kRunSlots + 1 && dg::CurrentStats().runsStale == 1 && dg::CurrentStats().runTableFull == 2);
  End();
}

// The stats line counts the runs and the target-set events, so nothing is lost after a cap.
void TheStatsLineCountsRunsAndTargetSets() {
  const FollowFn hook = Installed(false);
  World(0);
  for (int frame = 0; frame < 100; ++frame) {
    Put64(0x140, frame % 2 == 0 ? 0 : kTarget);
    hook(g_cs, 1);
  }
  QCHECK(dg::CurrentStats().runs == 50 && dg::CurrentStats().targetSets == 50);
  dg::NoteEnd();
  const std::vector<Line> stats = Lines("combat_droneguard_stats");
  QCHECK(stats.size() == 1U && Has(stats[0].text, "\"runs\":50,\"target_set\":50"));
  End();
}

// 4. State lines: the first call and every change of (index, flags, target null, system null), at most 16; read-only.
void TheStateLineFollowsTheCacheAndIsCapped() {
  const FollowFn hook = Installed(false);
  World(kTarget);
  hook(g_cs, 1);
  hook(g_cs, 1);
  QCHECK(Lines("combat_droneguard_state").size() == 1U);
  const std::vector<Line> state = Lines("combat_droneguard_state");
  QCHECK(state.size() == 1U && state[0].level == sentinel::LogLevel::kInfo &&
         Has(state[0].text, "\"target\":\"set\",\"handle_sys\":\"set\",\"idx_unresolved\":\"no\",\"raw190\":\"00070003\",\"flags16c\":\"00\""));
  auto count = [] { return Lines("combat_droneguard_state").size(); };
  auto last = [] {
    const std::vector<Line> l = Lines("combat_droneguard_state");
    return l.empty() ? std::string() : l.back().text;
  };
  Put32(0x190, 0x0007ffff);
  hook(g_cs, 1);
  QCHECK(count() == 2U && Has(last(), "\"idx_unresolved\":\"yes\",\"raw190\":\"0007ffff\""));
  g_cs[0x16c] = 2;
  hook(g_cs, 1);
  QCHECK(Has(last(), "\"flags16c\":\"02\""));
  Put64(0x188, 0);
  hook(g_cs, 1);
  QCHECK(Has(last(), "\"handle_sys\":\"0\""));
  Put64(0x140, 0);
  hook(g_cs, 1);
  QCHECK(Has(last(), "\"target\":\"0\",\"handle_sys\":\"0\""));
  for (std::uint32_t i = 0; i < 40; ++i) {
    Put32(0x190, 0x00100000 + i);
    hook(g_cs, 1);
  }
  QCHECK(count() == 16U);
  QCHECK(Lines("combat_droneguard_log_cap").size() == 1U && Has(Lines("combat_droneguard_log_cap")[0].text, "\"lines\":\"state\",\"cap\":16"));  // the 17th change said so, once
  End();
}

// 5. Doubt: a null component goes to Follow, and the guard reads nothing else. (An unreadable non-null component faults in Follow's own first
// instruction, `ldr x8,[x0,#0xc8]`, with or without the guard.)
void ANullSystemGoesToFollow() {
  const FollowFn hook = Installed(true);
  World(kTarget);
  hook(nullptr, 9);
  QCHECK(g_realCalls == 1 && g_refreshCalls == 0 && dg::CurrentStats().skips == 0);
  End();
}

// ---- stage 1 ----

void AHealthyPlayerIsRefreshedThenChecked() {
  const FollowFn hook = Installed(true);
  const std::vector<Line> stage1 = Lines("combat_droneguard_stage1");
  QCHECK(stage1.size() == 1U && stage1[0].level == sentinel::LogLevel::kInfo && Has(stage1[0].text, "\"result\":\"on\""));
  const std::vector<Line> install = Lines("combat_droneguard_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"stage1\":\"on\""));
  World(kTarget);
  hook(g_cs, 0x77);
  QCHECK(g_refreshCalls == 1 && g_refreshArg == g_cs + 0x148);
  QCHECK(g_validCalls == 1 && g_validSys == reinterpret_cast<const void*>(kTarget) && g_validIdx == 0x00070003);
  QCHECK(g_realCalls == 1 && g_realHandle == 0x77 && dg::CurrentStats().idxSkips == 0);
  // the null half still comes first: no refresh when the target is null
  World(0);
  hook(g_cs, 1);
  QCHECK(g_refreshCalls == 0 && g_realCalls == 0);
  End();
}

// The index word after the refresh decides: the cache the refresh just wrote, not the one before.
void TheCacheTheRefreshWroteDecides() {
  const FollowFn hook = Installed(true);
  World(kTarget);
  g_afterIdx = 0x0007ffff;  // the refresh leaves the index unresolved
  hook(g_cs, 1);
  QCHECK(g_realCalls == 0 && g_validCalls == 0 && dg::CurrentStats().idxSkips == 1);
  const std::vector<Line> first = Lines("combat_droneguard_idx_skip");
  QCHECK(first.size() == 1U && first[0].level == sentinel::LogLevel::kInfo && Has(first[0].text, "\"why\":\"idx ffff\""));
  World(kTarget);
  g_afterIdx = 0xffff;  // only the low 16 bits count: generation 0 and index ffff
  hook(g_cs, 1);
  QCHECK(g_realCalls == 0);
  World(kTarget);
  g_afterIdx = 0x0001fffe;  // not unresolved
  hook(g_cs, 1);
  QCHECK(g_realCalls == 1);
  World(kTarget);
  g_afterSys = 0;  // the cached system is 0
  hook(g_cs, 1);
  QCHECK(g_realCalls == 0 && g_validCalls == 0);
  const std::vector<Line> reasons = Lines("combat_droneguard_idx_skip");
  QCHECK(!reasons.empty() && Has(reasons.back().text, "\"why\":\"handle-sys 0\""));
  End();
}

// IsComponentValid refuses the cached index: skipped; resolved again: one resolved line, once.
void ARefusedIndexIsSkippedUntilThePlayerResolves() {
  const FollowFn hook = Installed(true);
  World(kTarget);
  hook(g_cs, 1);  // a healthy call: no run of idx skips yet
  World(kTarget);
  g_valid = false;
  hook(g_cs, 1);
  QCHECK(g_realCalls == 0 && g_validCalls == 1);
  const std::vector<Line> skip = Lines("combat_droneguard_idx_skip");
  QCHECK(skip.size() == 1U && Has(skip[0].text, "\"why\":\"refused but cached\""));
  combat_test::Captured().clear();
  World(kTarget);
  hook(g_cs, 1);
  QCHECK(g_realCalls == 1);
  const std::vector<Line> resolved = Lines("combat_droneguard_idx_resolved");
  QCHECK(resolved.size() == 1U && resolved[0].level == sentinel::LogLevel::kInfo && Has(resolved[0].text, "\"idx_skips\":1"));
  hook(g_cs, 1);
  QCHECK(Lines("combat_droneguard_idx_resolved").size() == 1U);
  End();
}

// The cache becomes unreadable right after the refresh: doubt, Follow runs.
void ACacheThatTurnsUnreadableAfterTheRefreshGoesToFollow() {
  const FollowFn hook = Installed(true);
  auto* pg = static_cast<std::uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  World(kTarget);
  std::memcpy(pg + 0x140, g_cs + 0x140, 0x60);
  g_poison = pg;
  hook(pg, 1);
  g_poison = nullptr;
  QCHECK(g_realCalls == 1 && g_refreshCalls == 1 && dg::CurrentStats().idxSkips == 0);
  munmap(pg, 4096);
  End();
}

// The stage-1 table is as bounded as the null-target one: a 17th component with a stale cache finds it full, and the one Warning names it.
void AFullStageOneTableSaysSo() {
  const FollowFn hook = Installed(true);
  World(kTarget);
  g_valid = false;
  for (auto& comp : g_many) std::memcpy(comp, g_cs, sizeof(g_cs));
  for (unsigned i = 0; i <= dg::kRunSlots; ++i) hook(g_many[i], 1);
  QCHECK(g_realCalls == 0 && dg::CurrentStats().idxSkips == dg::kRunSlots + 1 && dg::CurrentStats().runTableFull == 1);
  const std::vector<Line> full = Lines("combat_droneguard_run_table_full");
  QCHECK(full.size() == 1U && Has(full[0].text, "\"table\":\"idx_skip\""));
  End();
}

// Idx skip totals: every 600th, at most 8.
void TheIdxSkipTotalsAreEvery600thAtMostEight() {
  const FollowFn hook = Installed(true);
  World(kTarget);
  g_afterIdx = 0xffff;
  for (int i = 0; i < 600; ++i) hook(g_cs, 1);
  const std::vector<Line> totals = Lines("combat_droneguard_idx_skips");
  QCHECK(totals.size() == 1U && Has(totals[0].text, "\"idx_skips\":600"));
  for (int i = 0; i < 600 * 9; ++i) hook(g_cs, 1);
  QCHECK(Lines("combat_droneguard_idx_skips").size() == 8U);
  QCHECK(Lines("combat_droneguard_log_cap").size() == 1U && Has(Lines("combat_droneguard_log_cap")[0].text, "\"lines\":\"idx_skips\",\"cap\":8"));  // the 9th total said they stopped
  End();
}

// Stage 1's run lines are capped like the null half's: a component flapping between a refused cache and a resolved one writes at most 8 of each, and says so once.
void AFlappingCacheIsCappedAndTheCapSaysSo() {
  const FollowFn hook = Installed(true);
  World(kTarget);
  for (int frame = 0; frame < 1000; ++frame) {
    g_afterIdx = frame % 2 == 0 ? 0x0007ffff : 0x00070003;
    hook(g_cs, 1);
  }
  QCHECK(Lines("combat_droneguard_idx_skip").size() == 8U && Lines("combat_droneguard_idx_resolved").size() == 8U);
  int skipCap = 0, resolvedCap = 0;
  for (const Line& l : Lines("combat_droneguard_log_cap")) {
    skipCap += Has(l.text, "\"lines\":\"idx_skip\",\"cap\":8") ? 1 : 0;
    resolvedCap += Has(l.text, "\"lines\":\"idx_resolved\",\"cap\":8") ? 1 : 0;
  }
  QCHECK(skipCap == 1 && resolvedCap == 1 && dg::CurrentStats().idxSkips == 500);
  End();
}

// Stage 1 asked, engine functions missing or half missing: one Error line, the null half only.
void StageOneWithoutItsFunctionsLeavesTheNullHalfOnly() {
  Begin();
  ModuleResult r = Install(true, true, nullptr);
  QCHECK(r.installed && std::strcmp(r.status, "installed_null_half") == 0 && Hooked());
  std::vector<Line> stage1 = Lines("combat_droneguard_stage1");
  QCHECK(stage1.size() == 1U && stage1[0].level == sentinel::LogLevel::kError && Has(stage1[0].text, "\"result\":\"null_half_only\"") &&
         Has(stage1[0].text, "\"reason\":\"engine_functions_missing\""));
  std::vector<Line> install = Lines("combat_droneguard_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo && Has(install[0].text, "\"stage1\":\"null_half_only\""));
  World(kTarget);
  Slot()(g_cs, 1);
  QCHECK(g_realCalls == 1 && g_refreshCalls == 0);
  End();
  Begin();
  dg::EngineFns half{};
  half.refresh = &FRefresh;
  r = Install(true, true, &half);
  QCHECK(r.installed && std::strcmp(r.status, "installed_null_half") == 0);
  World(kTarget);
  Slot()(g_cs, 1);
  QCHECK(g_realCalls == 1 && g_refreshCalls == 0);
  End();
  Begin();
  half = dg::EngineFns{};
  half.isValid = &FValid;
  r = Install(true, true, &half);
  QCHECK(r.installed && std::strcmp(r.status, "installed_null_half") == 0 && Lines("combat_droneguard_stage1").size() == 1U);
  End();
}

// A changed (or unreadable) stage-1 word leaves the null half only and names the pin.
void AChangedStageOneWordLeavesTheNullHalfOnlyAndNamesIt() {
  for (std::size_t i = 0; i < dg::kIdxWordCount; ++i) {
    Begin();
    const std::uint32_t good = g_image.Get<std::uint32_t>(dg::kIdxWords[i].vaddr);
    const std::uint32_t flipped = good ^ 0x00010000U;
    g_image.Put<std::uint32_t>(dg::kIdxWords[i].vaddr, flipped);
    const dg::EngineFns f = Fns();
    const ModuleResult r = Install(true, true, &f);
    QCHECK(r.installed && std::strcmp(r.status, "installed_null_half") == 0 && Hooked());
    const std::vector<Line> stage1 = Lines("combat_droneguard_stage1");
    QCHECK(stage1.size() == 1U);
    if (stage1.size() == 1U) {
      char at[19];
      char found[19];
      char want[19];
      QCHECK(stage1[0].level == sentinel::LogLevel::kError && Has(stage1[0].text, "\"reason\":\"pinned_word_mismatch\""));
      QCHECK(Has(stage1[0].text, std::string("\"pin\":\"") + dg::kIdxWords[i].what + "\""));
      QCHECK(Has(stage1[0].text, std::string("\"vaddr\":\"") + sentinel::HexString(at, dg::kIdxWords[i].vaddr) + "\""));
      QCHECK(Has(stage1[0].text, std::string("\"found\":\"") + sentinel::HexString(found, flipped) + "\""));
      QCHECK(Has(stage1[0].text, std::string("\"expected\":\"") + sentinel::HexString(want, good) + "\""));
    }
    World(kTarget);
    Slot()(g_cs, 1);
    QCHECK(g_realCalls == 1 && g_refreshCalls == 0 && g_validCalls == 0);  // the null half works, stage 1 does not run
    End();
  }
  {  // an unreadable stage-1 word (IsComponentValid's page, away from the stage-0 words)
    Begin();
    const std::uintptr_t page = (g_image.Base() + dg::kIdxWords[1].vaddr) & ~static_cast<std::uintptr_t>(0xfff);
    QCHECK(mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_NONE) == 0);
    const dg::EngineFns f = Fns();
    const ModuleResult r = Install(true, true, &f);
    mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE);
    QCHECK(r.installed && std::strcmp(r.status, "installed_null_half") == 0);
    const std::vector<Line> stage1 = Lines("combat_droneguard_stage1");
    QCHECK(stage1.size() == 1U && Has(stage1[0].text, "\"found\":\"unreadable\""));
    End();
  }
}

// The address check on what dlsym would return, and the resolver on a host with no game library.
void TheEnginesExportsMustBeAtTheirPinnedAddresses() {
  dg::EngineFns got{};
  const std::uintptr_t b = 0x7000000000ULL;
  QCHECK(dg::EngineExportsMatch(b, reinterpret_cast<void*>(b + dg::kRefreshCacheVaddr), reinterpret_cast<void*>(b + dg::kIsValidVaddr), &got) &&
         got.refresh == reinterpret_cast<void (*)(void*)>(b + dg::kRefreshCacheVaddr) &&
         got.isValid == reinterpret_cast<bool (*)(const void*, std::uint64_t)>(b + dg::kIsValidVaddr));
  QCHECK(!dg::EngineExportsMatch(b, reinterpret_cast<void*>(b + 0x1210840), reinterpret_cast<void*>(b + dg::kIsValidVaddr), &got));
  QCHECK(!dg::EngineExportsMatch(b, reinterpret_cast<void*>(b + dg::kRefreshCacheVaddr), reinterpret_cast<void*>(b + 0x1cdb3e8), &got));
  QCHECK(!dg::EngineExportsMatch(b, nullptr, nullptr, &got));
  QCHECK(!dg::ResolveEngineFns(b, &got));  // no libr15 is loaded in this process
}

// A slot that does not hold Follow is refused and left alone; so is a word that points at other code.
void ASlotThatDoesNotHoldFollowIsRefusedAndLeftAlone() {
  Begin();
  Build(0x4242420);
  ModuleResult r = Install(true, false, nullptr);
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(0x4242420));
  const std::vector<Line> install = Lines("combat_droneguard_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"result\":\"refused\"") &&
         Has(install[0].text, "\"reason\":\"slot_refused\"") && Has(install[0].text, "\"hook\":\"original_mismatch\""));
  End();
  Begin();
  Build(reinterpret_cast<std::uintptr_t>(&OtherFollow));
  r = Install(true, false, nullptr);
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&OtherFollow));
  End();
}

// A refused install leaves no stage 1 behind: a later install of the null half alone does not run it.
void ARefusedInstallLeavesNoStageOneBehind() {
  Begin();
  Build(0x4242420);
  const dg::EngineFns f = Fns();
  QCHECK(!Install(true, true, &f).installed);
  Build();  // the slot holds Follow now
  QCHECK(Install(true, false, nullptr).installed);
  World(kTarget);
  Slot()(g_cs, 1);
  QCHECK(g_realCalls == 1 && g_refreshCalls == 0 && g_validCalls == 0);
  End();
}

// The switch: off installs nothing, tries no stage 1 and says so.
void TheSwitchOffInstallsNothingAndSaysSo() {
  Begin();
  const dg::EngineFns f = Fns();
  const ModuleResult r = Install(false, true, &f);  // stage 1 asked, but the guard is off
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0 && SlotIsTheGames());
  QCHECK(Lines("combat_droneguard_stage1").empty());
  const std::vector<Line> install = Lines("combat_droneguard_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  if (install.size() == 1U) {
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_droneguard_install\",\"result\":\"off\",\"reason\":\"off_config\",\"droneguard_stage1\":1,\"consequence\":\"combat.droneguard is false: the "
           "game's own Follow runs for every component, and a drone with no orbit target faults it\"}");
  }
  End();
}

// The image: not loaded, no build ID, another build.
void AMissingOrOtherImageInstallsNothing() {
  Begin();
  QCHECK(std::strcmp(dg::InstallAt(nullptr, g_pins, true, false, nullptr).status, "module_not_loaded") == 0);
  g_image.Drop();
  QCHECK(std::strcmp(Install(true, false, nullptr).status, "module_not_loaded") == 0);
  Build();
  g_image.Build({}, pinned::kLibR15StockBuildId);
  QCHECK(std::strcmp(Install(true, false, nullptr).status, "build_id_mismatch") == 0);
  g_image.Build({}, nullptr, /*note=*/false);
  QCHECK(std::strcmp(Install(true, false, nullptr).status, "no_build_id") == 0);
  const std::vector<Line> lines = Lines("combat_droneguard_install");
  QCHECK(lines.size() == 4U);
  for (const Line& l : lines) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();
}

// The stage-0 words: every one flipped in turn (and an unreadable one) refuses the install before the slot is touched.
void EveryPinnedWordGatesTheInstall() {
  Begin();
  combat_test::EveryPinIsRefused(
      "combat_droneguard_install", kAllWords, g_image,
      [] {
        combat_test::StartCapture();
        dg::ResetForTest();
        Build();
      },
      [] { return Install(true, true, nullptr); }, [] { return SlotIsTheGames(); });
  End();
}

// A second install: the slot already holds the hook, nothing is installed again.
void ASecondInstallDoesNothing() {
  Begin();
  QCHECK(Install(true, false, nullptr).installed);
  const ModuleResult again = Install(true, false, nullptr);
  QCHECK(!again.installed && Hooked());
  End();
}

// The guard acts in every level: it has no level scope, so the model of an arena level changes nothing (the target is null, Follow is not called).
void TheGuardHasNoLevelScope() {
  const FollowFn hook = Installed(false);
  World(0);
  hook(g_cs, 1);  // no scope::Tick has ever run in this process image of the test: a level-scoped guard would let this through
  QCHECK(g_realCalls == 0 && dg::CurrentStats().skips == 1);
  End();
}

// The stats: a Debug line every kStatsEvery calls, the Info line at the end of the process only when the guard is installed.
void TheStatsAreWrittenEveryKStatsEveryCallsAndAtTheEnd() {
  const FollowFn hook = Installed(false);
  World(kTarget);
  for (unsigned i = 0; i < dg::kStatsEvery - 1; ++i) hook(g_cs, 1);
  QCHECK(Lines("combat_droneguard_stats").empty());
  hook(g_cs, 1);
  std::vector<Line> stats = Lines("combat_droneguard_stats");
  QCHECK(stats.size() == 1U && stats[0].level == sentinel::LogLevel::kDebug && Has(stats[0].text, "\"final\":0") &&
         Has(stats[0].text, "\"calls\":16384,\"skips\":0,\"idx_skips\":0"));
  dg::NoteEnd();
  stats = Lines("combat_droneguard_stats");
  QCHECK(stats.size() == 2U && stats[1].level == sentinel::LogLevel::kInfo && Has(stats[1].text, "\"final\":1"));
  End();
  Begin();  // not installed: no final line
  dg::NoteEnd();
  QCHECK(Lines("combat_droneguard_stats").empty());
  End();
}

// The final stats count both kinds of skip.
void TheFinalStatsCountBothKindsOfSkip() {
  const FollowFn hook = Installed(true);
  World(0);
  hook(g_cs, 1);
  hook(g_cs, 1);
  World(kTarget);
  g_afterIdx = 0xffff;
  hook(g_cs, 1);
  hook(g_cs, 1);
  hook(g_cs, 1);
  QCHECK(dg::CurrentStats().skips == 2 && dg::CurrentStats().idxSkips == 3 && dg::CurrentStats().calls == 5);
  dg::NoteEnd();
  const std::vector<Line> stats = Lines("combat_droneguard_stats");
  QCHECK(stats.size() == 1U && Has(stats[0].text, "\"calls\":5,\"skips\":2,\"idx_skips\":3,\"runs\":"));
  End();
}

}  // namespace

// Every combat_droneguard_install line names the stage-1 switch the step was handed (#2382), on every path, with stage 1 handed on (its default is
// off, so a line that drops it shows): installed, refused for want of libr15, and refused for a pinned word that differs. The off line is the golden
// line of TheSwitchOffInstallsNothingAndSaysSo.
// Both values, on every path (#2382 review). The slot-refused line is the one whose installed flag is false while the switch was handed on, so a
// line that reads "installed and stage 1" shows there; the off line is checked with the value handed off as well as on.
void EveryInstallLineNamesTheStageOneSwitchItWasHanded() {
  for (const bool s1 : {false, true}) {
    const std::string named = std::string("\"droneguard_stage1\":") + (s1 ? "1" : "0");
    Installed(s1);
    std::vector<Line> install = Lines("combat_droneguard_install");
    QCHECK(install.size() == 1U && Has(install[0].text, "\"result\":\"installed\"") && Has(install[0].text, named));
    End();
    Begin();
    const dg::EngineFns f = Fns();
    QCHECK(std::strcmp(dg::InstallAt(nullptr, g_pins, true, s1, &f).status, "module_not_loaded") == 0);
    install = Lines("combat_droneguard_install");
    QCHECK(install.size() == 1U && Has(install[0].text, "\"reason\":\"module_not_loaded\"") && Has(install[0].text, named));
    End();
    Begin();
    g_image.Put<std::uint32_t>(dg::kWords[0].vaddr, dg::kWords[0].word ^ 0x00010000U);
    QCHECK(std::strcmp(Install(true, s1, &f).status, "pinned_word_mismatch") == 0);
    install = Lines("combat_droneguard_install");
    QCHECK(install.size() == 1U && Has(install[0].text, "\"reason\":\"pinned_word_mismatch\"") && Has(install[0].text, named));
    End();
    Begin();
    Build(0x4242420);  // a slot that holds something other than Follow: the install is refused with its installed flag false
    QCHECK(!Install(true, s1, nullptr).installed);
    install = Lines("combat_droneguard_install");
    QCHECK(install.size() == 1U && Has(install[0].text, "\"reason\":\"slot_refused\"") && Has(install[0].text, named));
    End();
    Begin();
    QCHECK(Install(false, s1, &f).off);
    install = Lines("combat_droneguard_install");
    QCHECK(install.size() == 1U && Has(install[0].text, "\"result\":\"off\"") && Has(install[0].text, named));
    End();
  }
}

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheThunkTakesTheSlotAndTheLineSaysSo();
  ASetTargetReachesTheGamesFollowUntouched();
  ANullTargetIsNeverFollowedAndIsCounted();
  TheTargetArrivingEndsTheRunOfSkips();
  TwoComponentsEachCalledEveryFrameWriteOneSkipLineAndNoTargetSetLine();
  TwoComponentsWithNoTargetEachWriteTheirOwnSkipLine();
  WithNoRunOpenACallWithATargetScansNoTable();
  ARunOfAComponentThatDiedIsStaleAndItsTargetSetLineIsNotWritten();
  ADeadRunIsReleasedByTheScanAndTheEarlyOutComesBack();
  AFullTableSaysSoAndAStaleSlotIsReclaimed();
  TheStatsLineCountsRunsAndTargetSets();
  AFlappingComponentIsCappedAndTheCapSaysSo();
  TheStateLineFollowsTheCacheAndIsCapped();
  AFullStageOneTableSaysSo();
  ANullSystemGoesToFollow();
  AHealthyPlayerIsRefreshedThenChecked();
  TheCacheTheRefreshWroteDecides();
  ARefusedIndexIsSkippedUntilThePlayerResolves();
  ACacheThatTurnsUnreadableAfterTheRefreshGoesToFollow();
  TheIdxSkipTotalsAreEvery600thAtMostEight();
  AFlappingCacheIsCappedAndTheCapSaysSo();
  StageOneWithoutItsFunctionsLeavesTheNullHalfOnly();
  AChangedStageOneWordLeavesTheNullHalfOnlyAndNamesIt();
  TheEnginesExportsMustBeAtTheirPinnedAddresses();
  ASlotThatDoesNotHoldFollowIsRefusedAndLeftAlone();
  ARefusedInstallLeavesNoStageOneBehind();
  TheSwitchOffInstallsNothingAndSaysSo();
  AMissingOrOtherImageInstallsNothing();
  EveryPinnedWordGatesTheInstall();
  EveryInstallLineNamesTheStageOneSwitchItWasHanded();
  ASecondInstallDoesNothing();
  TheGuardHasNoLevelScope();
  TheStatsAreWrittenEveryKStatsEveryCallsAndAtTheEnd();
  TheFinalStatsCountBothKindsOfSkip();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "droneguard_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("droneguard_test: all checks passed");
  return 0;
}
