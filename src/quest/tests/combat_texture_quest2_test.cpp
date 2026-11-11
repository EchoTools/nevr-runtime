// Host test of the Quest 2 step-down's combat input (src/quest/combat/texture_quest2.cpp, #1737, docs/quest/combat/texture_quest2.md): the row's
// install (off by its key, refused when combat is off or the tick cannot run the look, else the tick callback and the owner's input registered),
// the look it publishes from a model of CR15Game (the loaded tables' class, an Echo Arena level winning, and the init-fiber count at game + 0x1f08:
// not 0, or unreadable, is a load) at most every 500 ms, and its lines. The owner's use of the look is texture_menu_hook_test's.
//
// Built and run by `just test-quest-combat texture_quest2`.
#include "quest/combat/texture_quest2.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "quest/combat/combat_active.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/combat_tick_targets.h"
#include "quest/sentinel/pinned_targets.h"
#include "quest/sentinel/texture_quality_input.h"
#include "quest/tests/combat_game_model.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/test_check.h"

namespace {

namespace tq2 = nevr_quest::combat::texture_quest2;
namespace tick = nevr_quest::combat::tick;
namespace combat = nevr_quest::combat;
namespace bv = nevr_quest::build_variant;
namespace rules = nevr_quest::texture_quality::rules;
namespace qin = nevr_quest::texture_quality::input;
namespace pinned = sentinel::pinned;
using combat_test::Has;
using combat_test::Image;
using combat_test::Lines;

constexpr std::size_t kFiberCount = 0x1f08;  // CR15Game's init-fiber count, spelled here (combat_level_targets.h pins it)

Image g_image;
combat_test::GameModel g_model;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
void FakeTickFrame(void*) {}

std::int64_t g_nowMs = 5000;
std::int64_t Clock() noexcept { return g_nowMs; }

void SetFibers(std::uint64_t count) {
  std::memcpy(reinterpret_cast<unsigned char*>(g_model.Address()) + kFiberCount, &count, sizeof(count));
}

void Begin(bool active = true, bool tickInstalled = true) {
  combat_test::StartCapture();
  tq2::ResetForTest();
  tick::ResetForTest();
  combat::ResetForTest();
  g_model.Clear();
  g_nowMs = 5000;
  combat::SetVariant(active ? bv::Variant::kCombat : bv::Variant::kNone);
  combat::Resolve(/*devBuild=*/false, /*forceAsked=*/false);
  tq2::SetClockForTest(&Clock);
  namespace tt = tick::targets;
  g_image.Build({{tt::kSymbol, tt::kSlotVaddr, false, false, tt::kStockVaddr}});
  *g_image.Slot(tt::kSlotVaddr) = reinterpret_cast<void*>(&FakeTickFrame);
  if (tickInstalled) QCHECK(tick::InstallAt(&Lookup));
  combat_test::Captured().clear();
}
void End() {
  tq2::ResetForTest();
  tick::ResetForTest();
  g_image.Drop();
  combat_test::StopCapture();
}

// The tick looks (the clock moves past the look spacing first).
rules::Look LookNow() {
  g_nowMs += tq2::kLookEveryMs;
  tq2::Tick(g_model.Address());
  return tq2::CurrentLook();
}

void TheKeyOffRegistersNothing() {
  Begin();
  const std::size_t callbacks = tick::Count();
  const combat::ModuleResult r = tq2::Install(/*on=*/false, /*lobby=*/true);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off") == 0);
  QCHECK(qin::Registered() == nullptr && tick::Count() == callbacks);  // arena pays nothing: no callback, no input
  const auto lines = Lines("combat_texture_quest2_install");
  QCHECK(lines.size() == 1 && Has(lines[0].text, "\"result\":\"off\""));
  End();
}

void ItIsRefusedWhenCombatIsOffOrTheTickCannotLook() {
  Begin(/*active=*/false);
  combat::ModuleResult r = tq2::Install(true, true);
  QCHECK(!r.installed && !r.off && std::strcmp(r.status, "inactive") == 0 && qin::Registered() == nullptr);
  End();
  Begin(/*active=*/true, /*tickInstalled=*/false);
  r = tq2::Install(true, true);
  QCHECK(!r.installed && std::strcmp(r.status, "tick_not_installed") == 0 && qin::Registered() == nullptr);
  const auto lines = Lines("combat_texture_quest2_install");
  QCHECK(lines.size() == 1 && lines[0].level == sentinel::LogLevel::kError && Has(lines[0].text, "\"reason\":\"tick_not_installed\""));
  End();
}

void InstalledItRegistersTheTickAndTheOwnersInput() {
  Begin();
  const std::size_t callbacks = tick::Count();
  const combat::ModuleResult r = tq2::Install(true, /*lobby=*/false);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(tick::Count() == callbacks + 1);
  QCHECK(qin::Registered() == &tq2::CurrentLook);
  QCHECK(!qin::Switches().lobby);  // the lobby switch is handed over as given
  const auto lines = Lines("combat_texture_quest2_install");
  QCHECK(lines.size() == 1 && Has(lines[0].text, "\"result\":\"installed\"") && Has(lines[0].text, "\"lobby\":0"));
  End();
}

void TheLookNamesTheLoadedClassAndArenaWins() {
  Begin();
  QCHECK(tq2::Install(true, true).installed);
  rules::Look look = tq2::CurrentLook();
  QCHECK(look.atMs == 0 && look.loading);  // nothing seen yet: no look, and loading
  g_model.Only({"mpl_combat_dyson"});
  look = LookNow();
  QCHECK(look.where == rules::Where::kCombatMap && !look.loading && look.atMs == g_nowMs);
  g_model.Only({"mpl_lobby_b_combat"});
  QCHECK(LookNow().where == rules::Where::kCombatLobby);
  g_model.Only({"mpl_combat_gauss", "mpl_arena_a"});
  QCHECK(LookNow().where == rules::Where::kEchoArena);
  g_model.Only({"mpl_lobby_b2"});
  QCHECK(LookNow().where == rules::Where::kNone);
  End();
}

void AnInitFiberOrAnUnreadableCountIsALoad() {
  Begin();
  QCHECK(tq2::Install(true, true).installed);
  g_model.Only({"mpl_combat_fission"});
  SetFibers(2);
  rules::Look look = LookNow();
  QCHECK(look.where == rules::Where::kCombatMap && look.loading);
  SetFibers(0);
  QCHECK(!LookNow().loading);
  // A game object that cannot be read: the count is unknown, which is a load.
  g_nowMs += tq2::kLookEveryMs;
  tq2::Tick(0x10);
  look = tq2::CurrentLook();
  QCHECK(look.loading && look.where == rules::Where::kNone);
  End();
}

void ItLooksAtMostEveryHalfSecondAndLogsChangesOnly() {
  Begin();
  QCHECK(tq2::Install(true, true).installed);
  combat_test::Captured().clear();
  g_model.Only({"mpl_combat_dyson"});
  LookNow();
  const std::int64_t first = tq2::CurrentLook().atMs;
  g_nowMs += tq2::kLookEveryMs - 1;
  tq2::Tick(g_model.Address());
  QCHECK(tq2::CurrentLook().atMs == first);  // too soon: no look
  for (int i = 0; i < 5; ++i) LookNow();
  QCHECK(Lines("combat_texture_quest2_look").size() == 1);  // the same class, no load: one line
  SetFibers(1);
  LookNow();
  SetFibers(0);
  LookNow();
  const auto lines = Lines("combat_texture_quest2_look");
  QCHECK(lines.size() == 3);
  QCHECK(Has(lines[1].text, "\"loading\":1") && Has(lines[1].text, "\"fibers\":1") && Has(lines[2].text, "\"loading\":0"));
  End();
}

}  // namespace

int main() {
  TheKeyOffRegistersNothing();
  ItIsRefusedWhenCombatIsOffOrTheTickCannotLook();
  InstalledItRegistersTheTickAndTheOwnersInput();
  TheLookNamesTheLoadedClassAndArenaWins();
  AnInitFiberOrAnUnreadableCountIsALoad();
  ItLooksAtMostEveryHalfSecondAndLogsChangesOnly();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_texture_quest2_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::printf("combat_texture_quest2_test: ok\n");
  return 0;
}
