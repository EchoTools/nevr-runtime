// Host test of the bullet-velocity guard (src/quest/combat/bullet_velocity_guard.cpp, #1737, #1651): the rule
// (runtime/combat/bullet_velocity_guard_rules.h), and the real handler through the real thunk, installed by the real GotHook into FireBullet's
// JUMP_SLOT of a libr15 image built in memory, called through the slot the way FireBulletLocal calls it.
//
// What it pins: the rule's table (null, zero, |v|^2 = 1e-6f exactly, NaN, inf skip; just above does not); a skipped call returns 0 and never reaches
// FireBullet; any other call reaches it with all eight arguments unchanged and its result is returned; the guard and dronemuzzle's assembly entry
// share the slot only with the guard installed first (the reverse is refused slot_held_by_entry), and a zero shot through the entry is still
// skipped; a slot holding another function and every normalise word flipped refuse the install with the slot untouched; the switch; the image
// refusals; the skip lines (10, then one notice, then a count at most every 10 s) and the end total.
//
// Built and run by `just test-quest-combat bullet_velocity_guard`.
#include "quest/combat/bullet_velocity_guard.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "quest/combat/dronekill.h"
#include "quest/combat/dronemuzzle.h"
#include "quest/sentinel/hook_install.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pin_check.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/test_check.h"
#include "runtime/combat/bullet_velocity_guard_rules.h"

namespace {

namespace bvg = nevr_quest::combat::bullet_velocity_guard;
namespace rules = nevr_combat::bullet_velocity_guard;
namespace dm = nevr_quest::combat::dronemuzzle;
namespace pinned = sentinel::pinned;
using combat_test::Has;
using combat_test::Image;
using combat_test::Line;
using combat_test::Lines;
using nevr_quest::combat::ModuleResult;
using sentinel::GotStatus;

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// The rule is constexpr: the table holds at compile time too.
constexpr float kZero[3] = {0, 0, 0};
constexpr float kOne[3] = {1, 0, 0};
static_assert(rules::Skips(kZero) && !rules::Skips(kOne), "the rule's ends");

using FireFn = std::uint64_t (*)(std::uint8_t*, std::uint64_t, const void*, float, const void*, const void*, std::uint64_t, std::uint64_t);

// ---- the stand-in for FireBullet -----------------------------------------------------------------------------------------------
struct Call {
  std::uint8_t* bcs;
  std::uint64_t handle;
  const void* gun;
  float damage;
  const void* quat;
  const void* vel;
  std::uint64_t remote, flag;
};
std::vector<Call> g_calls;
extern "C" __attribute__((noinline)) std::uint64_t RealFire(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float damage, const void* quat,
                                                           const void* vel, std::uint64_t remote, std::uint64_t flag) {
  g_calls.push_back({bcs, handle, gun, damage, quat, vel, remote, flag});
  return 0x5678;
}
extern "C" __attribute__((noinline)) std::uint64_t OtherFire(std::uint8_t*, std::uint64_t, const void*, float, const void*, const void*, std::uint64_t,
                                                            std::uint64_t) {
  return 0;
}
// dronemuzzle's other slot (WorldTeleport) needs a function for its entry to sit on.
extern "C" __attribute__((noinline)) std::uint64_t RealTeleport(void*, std::uint64_t, const std::uint8_t*, std::uint64_t) { return 0x1234; }

// ---- the libr15 image ----------------------------------------------------------------------------------------------------------
Image g_image;
bvg::Pins g_pins = bvg::kPins;
constexpr std::uint64_t kTpSlot = Image::kRwStart + 0x20, kFireSlot = Image::kRwStart + 0x28;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
std::uint64_t Off(const void* fn) { return reinterpret_cast<std::uintptr_t>(fn) - g_image.Base(); }

// A libr15 holding the three normalise words and FireBullet's slot (`slotWord`, the stand-in when null), and WorldTeleport's for dronemuzzle.
void Build(void* slotWord = nullptr) {
  g_image.Build({{dm::kTeleportSymbol, kTpSlot, false, false, 0}, {dm::kFireSymbol, kFireSlot, false, false, 0}});
  combat_test::PutPins(g_image, &nevr_quest::combat::bullet_velocity_guard::targets::kWordPins, 1);
  *g_image.Slot(kTpSlot) = reinterpret_cast<void*>(&RealTeleport);
  *g_image.Slot(kFireSlot) = slotWord != nullptr ? slotWord : reinterpret_cast<void*>(&RealFire);
  g_pins = {kFireSlot, Off(reinterpret_cast<void*>(&RealFire))};
}
FireFn Slot() { return reinterpret_cast<FireFn>(*g_image.Slot(kFireSlot)); }
bool Hooked() { return *g_image.Slot(kFireSlot) == bvg::FireThunk::EntryAddress(); }
bool SlotIsTheGames() { return *g_image.Slot(kFireSlot) == reinterpret_cast<void*>(&RealFire); }

std::uint64_t g_nowMs = 1000;
std::uint64_t Clock() { return g_nowMs; }

void Begin() {
  combat_test::StartCapture();
  bvg::ResetForTest();
  dm::ResetForTest();
  g_calls.clear();
  g_nowMs = 1000;
  bvg::SetClockForTest(&Clock);
  Build();
}
void End() {
  bvg::ResetForTest();
  dm::ResetForTest();
  g_image.Drop();
  combat_test::StopCapture();
}
ModuleResult Install() { return bvg::InstallAt(&Lookup, g_pins); }

std::uint8_t g_bcs[16];
const void* const kGun = reinterpret_cast<void*>(0x9a9a);
const void* const kQuat = reinterpret_cast<void*>(0x9b9b);
std::uint64_t Shoot(FireFn f, const float* v, std::uint64_t handle = 0x12345) { return f(g_bcs, handle, kGun, 2.5f, kQuat, v, 5, 6); }

// ---- the rule ------------------------------------------------------------------------------------------------------------------
void SkipsZeroNullNanInf() {
  const float skip[][3] = {{0, 0, 0}, {kNan, 0, 0}, {0, kNan, 0}, {0, 0, kNan}, {kInf, 0, 0}, {0, -kInf, 0}, {1e-4f, 0, 0}, {0, 0, -5e-4f}, {3e19f, 0, 0}};
  QCHECK(rules::Skips(nullptr));
  for (const auto& v : skip) QCHECK(rules::Skips(v));
  const float pass[][3] = {{1, 0, 0}, {1.001e-3f, 0, 0}, {0, 0, 120}, {-30, 4, 0.5f}, {0, 0, -1.001e-3f}};
  for (const auto& v : pass) QCHECK(!rules::Skips(v));
}
// |v|^2 equal to 1e-6f skips (the test is `>`), just above passes; the threshold is on the square, not on |v|. No float x has x*x == 1e-6f, so the
// boundary vector has two components whose squares sum to exactly 1e-6f (found by search). {1e-3f, 0, 0} itself passes: the float 1e-3f is a little
// above 1e-3, and its square a little above 1e-6f.
void ThresholdIsSquared() {
  QCHECK(rules::kMinSpeedSquared == 1e-6f);
  const float at[3] = {0x1.a36e2ep-11f, 0x1.3a92a4p-11f, 0}, above[3] = {1.0001e-3f, 0, 0}, milli[3] = {1e-3f, 0, 0}, below[3] = {0.999e-3f, 0, 0};
  QCHECK(at[0] * at[0] + at[1] * at[1] + at[2] * at[2] == 1e-6f);
  QCHECK(rules::Skips(at) && !rules::Skips(above) && !rules::Skips(milli) && rules::Skips(below));
  const float spread[3] = {6e-4f, 6e-4f, 6e-4f};  // each component below 1e-3, |v|^2 = 1.08e-6: passes
  QCHECK(!rules::Skips(spread));
}

// ---- the hook ------------------------------------------------------------------------------------------------------------------
void TheThunkTakesTheSlotAndTheLineSaysSo() {
  Begin();
  const ModuleResult r = Install();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off && Hooked());
  const std::vector<Line> lines = Lines("combat_bullet_velocity_guard_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    char slot[19];
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(lines[0].text) ==
           std::string("\"event\":\"combat_bullet_velocity_guard_install\",\"result\":\"installed\",\"reason\":\"ok\",\"slot\":\"") +
               sentinel::HexString(slot, kFireSlot) +
               "\",\"msg\":\"a FireBullet call whose velocity is null, at most about 1e-3 m/s or not finite is not made; every other call reaches FireBullet "
               "unchanged\"}");
  }
  End();
}

void SkipDoesNotCallOriginal() {
  Begin();
  QCHECK(Install().installed);
  const float zero[3] = {0, 0, 0}, nan[3] = {kNan, 1, 1}, slow[3] = {0, 5e-4f, 0};
  QCHECK(Shoot(Slot(), zero, 0xabcd0007) == 0);
  QCHECK(Shoot(Slot(), nan) == 0 && Shoot(Slot(), slow) == 0 && Shoot(Slot(), nullptr) == 0);
  QCHECK(g_calls.empty() && bvg::Skipped() == 4 && bvg::Calls() == 4);
  const std::vector<Line> skips = Lines("combat_bullet_velocity_guard_skip");
  QCHECK(skips.size() == 4U);
  if (skips.size() == 4U) {
    QCHECK(skips[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(skips[0].text) ==
           "\"event\":\"combat_bullet_velocity_guard_skip\",\"n\":1,\"vx\":\"0\",\"vy\":\"0\",\"vz\":\"0\",\"index\":7,\"msg\":\"a shot with no usable "
           "velocity was not fired: FireBullet would divide by zero and store a NaN direction\"}");
    QCHECK(Has(skips[1].text, "\"n\":2,\"vx\":\"nan\""));
    QCHECK(Has(skips[2].text, "\"vx\":\"0\",\"vy\":\"0.000500000024\""));
    QCHECK(Has(skips[3].text, "\"n\":4,\"vel\":\"null\",\"index\":"));
  }
  End();
}

void PassCallsOriginalUnchanged() {
  Begin();
  QCHECK(Install().installed);
  const float v[3] = {0, 0, 120};
  QCHECK(Shoot(Slot(), v, 0xfeed0042) == 0x5678);
  QCHECK(g_calls.size() == 1U);
  if (g_calls.size() == 1U) {
    const Call& c = g_calls[0];
    QCHECK(c.bcs == g_bcs && c.handle == 0xfeed0042 && c.gun == kGun && c.damage == 2.5f && c.quat == kQuat && c.vel == v && c.remote == 5 && c.flag == 6);
  }
  QCHECK(bvg::Skipped() == 0 && bvg::Calls() == 1 && Lines("combat_bullet_velocity_guard_skip").empty());
  End();
}

// The guard first, then dronemuzzle's entry above it: both install, the entry is the newest, and a zero shot from any caller still never reaches
// FireBullet. dronemuzzle first: the guard is refused slot_held_by_entry and the slot keeps the entry.
void InstallsBelowDronemuzzle() {
  const dm::Pins dmPins = {kTpSlot, 0, 0x10, kFireSlot, 0, 0x18};  // the call-site words never match a host return address
  {
    Begin();
    dm::Pins p = dmPins;
    p.teleportFn = Off(reinterpret_cast<void*>(&RealTeleport));
    p.fireFn = g_pins.fn;
    QCHECK(Install().installed);
    const dm::Result r = dm::InstallAt(&Lookup, p);
    QCHECK(r.ok && r.fire == GotStatus::kOk);
    QCHECK(*g_image.Slot(kFireSlot) == reinterpret_cast<void*>(&nevr_combat_dm_fire_entry));
    QCHECK(dm::WordsForTest().fireNext == reinterpret_cast<std::uintptr_t>(bvg::FireThunk::EntryAddress()));
    const float zero[3] = {0, 0, 0}, v[3] = {0, 0, 120};
    QCHECK(Shoot(Slot(), zero) == 0 && g_calls.empty() && bvg::Skipped() == 1);
    QCHECK(Shoot(Slot(), v) == 0x5678 && g_calls.size() == 1U);
    End();
  }
  {
    Begin();
    dm::Pins p = dmPins;
    p.teleportFn = Off(reinterpret_cast<void*>(&RealTeleport));
    p.fireFn = g_pins.fn;
    QCHECK(dm::InstallAt(&Lookup, p).ok);
    const ModuleResult r = Install();
    QCHECK(!r.installed && std::strcmp(r.status, "slot_held_by_entry") == 0);
    QCHECK(*g_image.Slot(kFireSlot) == reinterpret_cast<void*>(&nevr_combat_dm_fire_entry));
    const std::vector<Line> lines = Lines("combat_bullet_velocity_guard_install");
    QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kError && Has(lines[0].text, "\"result\":\"refused\",\"reason\":\"slot_held_by_entry\""));
    const float zero[3] = {0, 0, 0};
    QCHECK(Shoot(Slot(), zero) == 0x5678 && g_calls.size() == 1U);  // the refused guard is not in the chain: FireBullet is reached
    End();
  }
}

void RefusesForeignOriginal() {
  Begin();
  Build(reinterpret_cast<void*>(&OtherFire));
  g_pins.fn = Off(reinterpret_cast<void*>(&RealFire));
  const ModuleResult r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(kFireSlot) == reinterpret_cast<void*>(&OtherFire));
  const std::vector<Line> lines = Lines("combat_bullet_velocity_guard_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kError && Has(lines[0].text, "\"reason\":\"original_mismatch\""));
  End();
}

void RefusesChangedWord() {
  // the install checks the three words of kWordPins, all of them (a shortened kWordPins is seen here)
  QCHECK(bvg::targets::kWordCount == 3 && bvg::targets::kWordPins.wordCount == 3 && bvg::targets::kWordPins.words == bvg::targets::kWords);
  QCHECK(bvg::targets::kWords[0].vaddr == 0x210ccac && bvg::targets::kWords[0].word == 0x1e2e1003);
  QCHECK(bvg::targets::kWords[1].vaddr == 0x210ccc0 && bvg::targets::kWords[1].word == 0x1e21c084);
  QCHECK(bvg::targets::kWords[2].vaddr == 0x210ccc4 && bvg::targets::kWords[2].word == 0x1e241863);
  QCHECK(bvg::kPins.slot == 0x36f3cf8 && bvg::kPins.fn == 0x210c9c4);
  Begin();
  combat_test::EveryPinIsRefused(
      "combat_bullet_velocity_guard_install", bvg::targets::kWordPins, g_image,
      [] {
        combat_test::StartCapture();
        bvg::ResetForTest();
        Build();
      },
      [] { return Install(); }, [] { return SlotIsTheGames(); });
  End();
}

void TheSwitchOffInstallsNothingAndSaysSo() {
  Begin();
  bvg::SetInstallForTest(&Lookup, &g_pins);
  const ModuleResult r = bvg::Install(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off") == 0 && SlotIsTheGames());
  const std::vector<Line> lines = Lines("combat_bullet_velocity_guard_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kInfo && Has(lines[0].text, "\"result\":\"off\",\"reason\":\"off_config\""));
  const ModuleResult on = bvg::Install(true);  // the step's production path, on the test image
  QCHECK(on.installed && Hooked());
  End();
}

void AMissingOrOtherImageInstallsNothing() {
  Begin();
  QCHECK(std::strcmp(bvg::InstallAt(nullptr, g_pins).status, "module_not_loaded") == 0);
  g_image.Drop();
  QCHECK(std::strcmp(Install().status, "module_not_loaded") == 0);
  g_image.Build({}, pinned::kLibR15StockBuildId);
  QCHECK(std::strcmp(Install().status, "build_id_mismatch") == 0);
  g_image.Build({}, nullptr, /*note=*/false);
  QCHECK(std::strcmp(Install().status, "no_build_id") == 0);
  const std::vector<Line> lines = Lines("combat_bullet_velocity_guard_install");
  QCHECK(lines.size() == 4U);
  for (const Line& l : lines) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();
}

// 10 skip lines, then one notice, then a count line at most every 10 s while skips continue; the end total counts every one.
void TheSkipLinesAreBudgetedAndTotalled() {
  Begin();
  QCHECK(Install().installed);
  const float zero[3] = {0, 0, 0}, v[3] = {1, 2, 3};
  for (int i = 0; i < 10; ++i) Shoot(Slot(), zero);
  QCHECK(Lines("combat_bullet_velocity_guard_skip").size() == 10U && Lines("combat_bullet_velocity_guard_skips").empty());
  Shoot(Slot(), zero);  // the 11th: the notice
  std::vector<Line> skips = Lines("combat_bullet_velocity_guard_skips");
  QCHECK(skips.size() == 1U && skips[0].level == sentinel::LogLevel::kInfo && Has(skips[0].text, "\"n\":11,\"since_last\":0"));
  for (int i = 0; i < 5; ++i) Shoot(Slot(), zero);
  g_nowMs += 9999;
  Shoot(Slot(), zero);
  QCHECK(Lines("combat_bullet_velocity_guard_skips").size() == 1U);  // under 10 s since the notice: counted only
  g_nowMs += 1;
  Shoot(Slot(), zero);  // 10 s: the count of the 7 since the notice
  skips = Lines("combat_bullet_velocity_guard_skips");
  QCHECK(skips.size() == 2U && Has(skips[1].text, "\"n\":18,\"since_last\":7"));
  QCHECK(Lines("combat_bullet_velocity_guard_skip").size() == 10U);
  Shoot(Slot(), v);
  bvg::NoteEnd();
  const std::vector<Line> total = Lines("combat_bullet_velocity_guard_total");
  QCHECK(total.size() == 1U && total[0].level == sentinel::LogLevel::kInfo && Has(total[0].text, "\"skipped\":18,\"calls\":19"));
  End();
  Begin();  // not installed: no total
  bvg::NoteEnd();
  QCHECK(Lines("combat_bullet_velocity_guard_total").empty());
  End();
}

}  // namespace

int main() {
  SkipsZeroNullNanInf();
  ThresholdIsSquared();
  TheThunkTakesTheSlotAndTheLineSaysSo();
  SkipDoesNotCallOriginal();
  PassCallsOriginalUnchanged();
  InstallsBelowDronemuzzle();
  RefusesForeignOriginal();
  RefusesChangedWord();
  TheSwitchOffInstallsNothingAndSaysSo();
  AMissingOrOtherImageInstallsNothing();
  TheSkipLinesAreBudgetedAndTotalled();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "bullet_velocity_guard_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("bullet_velocity_guard_test: all checks passed");
  return 0;
}
