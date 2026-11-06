// Host test of the particle-renderer gate (src/quest/combat/pfx.cpp, #2188; moved from the q15t plugin's pfx, #1631). The decision is on
// by default and off only when `combat.pfx` is false; the gate reads off until the install step has decided; the plugin's `pfx.off` kill
// file is no longer read and one found is a Warning. New with the move: the gate is scoped to combat maps, because arena runs on the same
// APK, and a fake CR15Game (the two gamespace tables, read by process_vm_readv as in production) shows it shut on every Echo Arena level.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/pfx.h"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/test_check.h"

namespace {

namespace pfx = nevr_quest::combat::pfx;
using EchoVR::CSymbol64Hash;

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}
void Fresh() {
  g_lines.clear();
  pfx::ResetForTest();
  pfx::SetLegacyDirForTest(nullptr);
}

// CR15Game as the module reads it: the offsets are spelled here, not taken from the header (combat_level_test does the same).
constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  Gamespace spaces[8];
};
Game g_game;
std::uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(g_game.words); }
void LoadLevels(std::initializer_list<const char*> names) {
  g_game = Game{};
  int row = 0;
  for (const char* name : names) {
    Gamespace& gs = g_game.spaces[row];
    gs.words[kGamespaceOwner / 8] = GameAddress();
    gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(name));
    g_game.table0[row][0] = 0x5555;
    g_game.table0[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
    ++row;
  }
  g_game.words[kTable0 / 8] = reinterpret_cast<std::uintptr_t>(g_game.table0);
  g_game.words[kCount0 / 8] = static_cast<std::uintptr_t>(row);
}

// 1 and 2: the decision from the one fact, the key.
void TheDecisionIsOnByDefaultAndOffOnlyWithTheKeyFalse() {
  const pfx::GateDecision on = pfx::Decide(true);
  QCHECK(on.on);
  QCHECK(on.reason == pfx::GateReason::kDefault);
  const pfx::GateDecision off = pfx::Decide(false);
  QCHECK(!off.on);
  QCHECK(off.reason == pfx::GateReason::kConfigOff);
}

// 3 and 5: before the install the gate reads off; the reset forgets the decision.
void TheGateReadsOffUntilTheInstallHasDecided() {
  Fresh();
  QCHECK(!pfx::GateOn());
  QCHECK(pfx::Install(true).installed);
  QCHECK(pfx::GateOn());
  pfx::ResetForTest();
  QCHECK(!pfx::GateOn());
}

// 4: the install stores the decision and writes one line that names it.
void TheInstallStoresTheDecisionAndWritesOneLine() {
  Fresh();
  const nevr_quest::combat::ModuleResult r = pfx::Install(true);
  QCHECK(r.installed && std::string(r.status) == "installed");
  const std::vector<Line> lines = Lines("combat_pfx_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(lines[0].text, "\"state\":\"on\""));
    QCHECK(Has(lines[0].text, "\"reason\":\"default\""));
    QCHECK(Has(lines[0].text, "\"switch\":\"combat.pfx\""));
  }
  Fresh();
  const nevr_quest::combat::ModuleResult off = pfx::Install(false);
  QCHECK(!off.installed && std::string(off.status) == "off_config");
  QCHECK(!pfx::GateOn());
  const std::vector<Line> offLines = Lines("combat_pfx_install");
  QCHECK(offLines.size() == 1U);
  if (offLines.size() == 1U) {
    QCHECK(offLines[0].level == sentinel::LogLevel::kInfo);  // a choice, not a failure
    QCHECK(Has(offLines[0].text, "\"state\":\"off\"") && Has(offLines[0].text, "\"reason\":\"off_config\""));
  }
}

// The plugin's kill file is not read: it turns nothing off, and one found is a Warning naming the key that replaces it.
void TheKillFileIsNotReadAndIsWarnedAbout() {
  Fresh();
  char dir[] = "/tmp/pfx_legacy_XXXXXX";
  QCHECK(mkdtemp(dir) != nullptr);
  pfx::SetLegacyDirForTest(dir);
  pfx::Install(true);
  QCHECK(Lines("combat_pfx_legacy_switch").empty());  // no file, no line
  const std::string file = std::string(dir) + "/pfx.off";
  if (std::FILE* f = std::fopen(file.c_str(), "w")) std::fclose(f);
  Fresh();
  pfx::SetLegacyDirForTest(dir);
  pfx::Install(true);
  QCHECK(pfx::GateOn());  // the file changed nothing
  const std::vector<Line> w = Lines("combat_pfx_legacy_switch");
  QCHECK(w.size() == 1U);
  if (w.size() == 1U) {
    QCHECK(w[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(w[0].text, "\"file\":\"pfx.off\"") && Has(w[0].text, "combat.pfx: false"));
  }
  std::remove(file.c_str());
  rmdir(dir);
  pfx::SetLegacyDirForTest(nullptr);
}

// Arena shares the APK: the gate is open on a combat map only, and shut on every Echo Arena level and every other level.
void TheGateIsOpenOnACombatMapOnlyAndShutInArena() {
  Fresh();
  pfx::Install(true);
  for (const char* combat : {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss"}) {
    LoadLevels({combat});
    QCHECK(pfx::InCombatMap(GameAddress()));
    QCHECK(pfx::GateOpenIn(GameAddress()));
  }
  for (const char* other : {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b_social",
                            "mpl_lobby_b_arena", "mpl_lobby_b_combat"}) {
    LoadLevels({other});
    QCHECK(!pfx::InCombatMap(GameAddress()));
    QCHECK(!pfx::GateOpenIn(GameAddress()));
  }
  // An arena level still loaded during a transition wins over the combat map.
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});
  QCHECK(!pfx::GateOpenIn(GameAddress()));
  // An unreadable game is no level: shut.
  QCHECK(!pfx::GateOpenIn(0));
  // The switch off shuts it on a combat map too.
  pfx::Install(false);
  LoadLevels({"mpl_combat_dyson"});
  QCHECK(pfx::InCombatMap(GameAddress()) && !pfx::GateOpenIn(GameAddress()));
}

// Arena pays nothing at install: the step registers no tick and installs no hook, and it writes one line.
void TheInstallTouchesNoHookAndNoTick() {
  Fresh();
  pfx::Install(true);
  QCHECK(g_lines.size() == 1U);
}

// The cost of the two calls a consumer would make (ADR 0038): printed, not asserted, since a time is not a property of the code. On the host, in
// an arena level (the shut gate is the case that matters for arena), so the first caller starts from a number.
void PrintTheCostOfTheGateInAnArenaLevel() {
  Fresh();
  pfx::Install(true);
  LoadLevels({"mpl_arena_a"});
  constexpr int kCalls = 200000;
  volatile bool sink = false;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kCalls; ++i) sink = pfx::GateOn();
  auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < kCalls; ++i) sink = pfx::InCombatMap(GameAddress());
  auto t2 = std::chrono::steady_clock::now();
  (void)sink;
  const auto ns = [&](auto a, auto b) { return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()) / kCalls; };
  std::printf("pfx_test: cost on the host in an arena level: GateOn %.1f ns/call, InCombatMap (the level scan) %.1f ns/call, %d calls each\n",
              ns(t0, t1), ns(t1, t2), kCalls);
}

}  // namespace

int main() {
  const sentinel::LogSink previous = sentinel::SetLogSink(&Capture);
  TheDecisionIsOnByDefaultAndOffOnlyWithTheKeyFalse();
  TheGateReadsOffUntilTheInstallHasDecided();
  TheInstallStoresTheDecisionAndWritesOneLine();
  TheKillFileIsNotReadAndIsWarnedAbout();
  TheGateIsOpenOnACombatMapOnlyAndShutInArena();
  TheInstallTouchesNoHookAndNoTick();
  PrintTheCostOfTheGateInAnArenaLevel();
  sentinel::SetLogSink(previous);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "pfx_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::printf("pfx_test: all checks passed\n");
  return 0;
}
