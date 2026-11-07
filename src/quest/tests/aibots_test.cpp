// Host test of practice AI bots (src/quest/combat/aibots.cpp, #2185; moved from the q15t plugin's aibots) against a fake CR15Game / CR15NetGame /
// CR15NetAIScenarios. Real SafeRead, real request file, a fake clock; EnableScenario is a fake that records its calls.
//
// Moved: sections 0 to 11 are the plugin test's sections with the same assertions (the defaults, the 3 s hold and its exact timing, zero bots, not in
// game / not a combat match / no scenarios object / another game, the idle line, the count changing, the 4 Hz look, the re-call, the re-arm on leaving,
// the request file, the agent warning, the host and combat-map fields, the pinned address and first word, a missing library). What changed is the
// fixture (InstallResolved with the fake, the game now has levels) and the lines (`combat_aibots_*` records). The request file is read in a development
// build only, so section 7 installs as one. New with the move: 12 the switch, 13 `combat.aibots_scenario`, 14 a release build reads no request file and
// warns about one, 15 the Echo Arena scope, 16 the install on an image, 17 the host cost of a look.
//
// Built and run by `just test-quest-combat`.
#include "quest/combat/aibots.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/aibots_targets.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/netgame_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/test_check.h"

namespace {

namespace ai = nevr_quest::combat::aibots;
namespace at = nevr_quest::combat::aibots::targets;
using EchoVR::CSymbol64Hash;

#define CHECK(cond, ...)                                                           \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "%s:%d CHECK failed: %s  ", __FILE__, __LINE__, #cond); \
      std::fprintf(stderr, __VA_ARGS__);                                           \
      std::fprintf(stderr, "\n");                                                  \
      ++quest_test::Failures();                                                    \
    }                                                                              \
  } while (0)

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
int Count(const char* event, const char* needle = "", int level = -1) {
  int n = 0;
  for (const Line& l : Lines(event)) n += Has(l.text, needle) && (level < 0 || static_cast<int>(l.level) == level) ? 1 : 0;
  return n;
}
bool HasLine(const char* event, const char* needle = "", int level = -1) { return Count(event, needle, level) > 0; }
constexpr int kInfo = static_cast<int>(sentinel::LogLevel::kInfo), kWarn = static_cast<int>(sentinel::LogLevel::kWarn),
              kError = static_cast<int>(sentinel::LogLevel::kError);

const std::uintptr_t kBase = 0x7000000000ull;
template <typename T>
void Put(void* base, std::size_t off, T v) {
  std::memcpy(static_cast<char*>(base) + off, &v, sizeof v);
}

// ---- the fake world: the offsets are spelled here, not taken from the headers ----
alignas(16) std::uintptr_t g_game[0x9000 / 8];
alignas(16) std::uintptr_t g_net[0x64000 / 8];
alignas(16) std::uintptr_t g_scen[0x600 / 8];
alignas(16) std::uintptr_t g_flagsPtr[1];
alignas(16) std::uintptr_t g_table[8][2];
alignas(16) std::uintptr_t g_gs[4][0x100 / 8];

struct EnableCall {
  void* scen;
  int state;
  double at;
};
std::vector<EnableCall> g_calls;
double g_now = 100.0;
double Clock() { return g_now; }
bool g_enableSets = true;
void FEnable(void* scen, int state) {
  g_calls.push_back({scen, state, g_now});
  if (g_enableSets) Put<std::uintptr_t>(scen, 0x560, 0x1234);
}

void SetCount(std::uint32_t n) { Put<std::uint32_t>(g_scen, 0x500, n); }
void SetState(std::int32_t s) { Put<std::int32_t>(g_net, 0, s); }
// The levels the game holds (gs+0x58 of each gamespace of table 0).
void SetLevels(std::initializer_list<std::uint64_t> levels) {
  std::memset(g_table, 0, sizeof g_table);
  std::memset(g_gs, 0, sizeof g_gs);
  int row = 0;
  for (const std::uint64_t level : levels) {
    g_gs[row][0x8 / 8] = reinterpret_cast<std::uintptr_t>(g_game);
    g_gs[row][0x58 / 8] = static_cast<std::uintptr_t>(level);
    g_table[row][0] = 0x5555;
    g_table[row][1] = reinterpret_cast<std::uintptr_t>(g_gs[row]);
    ++row;
  }
  g_game[0x1f48 / 8] = reinterpret_cast<std::uintptr_t>(g_table);
  g_game[0x1f78 / 8] = static_cast<std::uintptr_t>(row);
}
void BuildWorld() {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_net, 0, sizeof g_net);
  std::memset(g_scen, 0, sizeof g_scen);
  g_flagsPtr[0] = 0x40;  // a host
  g_game[0x8510 / 8] = reinterpret_cast<std::uintptr_t>(g_net);
  Put<std::uintptr_t>(g_net, 8, reinterpret_cast<std::uintptr_t>(g_game));
  SetState(9);
  Put<std::uint8_t>(g_net, 0x6341b, 1);
  Put<std::uintptr_t>(g_net, 0x2ab0, reinterpret_cast<std::uintptr_t>(g_scen));
  Put<std::uintptr_t>(g_net, 0x2da0, reinterpret_cast<std::uintptr_t>(g_flagsPtr));
  SetCount(3);
  SetLevels({0x1111});  // one gamespace holding no combat level
}

std::string g_dir;
time_t g_mt = 1000000;
void ClearDir() {
  DIR* d = opendir(g_dir.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] != '.') unlink((g_dir + "/" + e->d_name).c_str());
  }
  closedir(d);
}
void WriteReq(const char* text) {
  const std::string p = g_dir + "/aibots.req";
  std::FILE* f = std::fopen(p.c_str(), "w");
  std::fputs(text, f);
  std::fclose(f);
  g_mt += 10;  // an mtime that differs from the last one however fast the test runs
  timespec ts[2] = {{g_mt, 0}, {g_mt, 0}};
  utimensat(AT_FDCWD, p.c_str(), ts, 0);
}

ai::Fns Fns() {
  ai::Fns f{};
  f.enable = &FEnable;
  return f;
}
// The module as the install step leaves it, with the fake standing in for EnableScenario. `dev`: a development build (the request file is polled).
nevr_quest::combat::ModuleResult Reset(bool dev = false, const char* scenario = nullptr, bool on = true, const char* request = nullptr) {
  g_lines.clear();
  ClearDir();
  if (request != nullptr) WriteReq(request);
  BuildWorld();
  g_calls.clear();
  g_enableSets = true;
  g_now = 100.0;
  nevr_quest::combat::tick::ResetForTest();
  ai::ResetForTest();
  ai::SetClockForTest(&Clock);
  ai::SetLegacyDirForTest(g_dir.c_str());
  ai::Switches sw;
  sw.on = on;
  sw.scenario = scenario;
  sw.devBuild = dev;
  return ai::InstallResolved(kBase, Fns(), sw);
}
void Tick() {
  ai::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  g_now += 0.0625;
}
void Run(double seconds) {
  const double end = g_now + seconds;
  while (g_now < end) Tick();
}

std::uint32_t g_word[4];
void* g_lookupRet = nullptr;
void* Lookup(const char*) { return g_lookupRet; }

void TheDefaults() {  // 0, 1
  Reset();
  Run(4);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 1 && g_calls[0].at == 103.0, "defaults: %zu calls, state %d at %.4f", g_calls.size(), g_calls.empty() ? -9 : g_calls[0].state,
        g_calls.empty() ? 0.0 : g_calls[0].at);
  // three bots, held for 3 s, EnableScenario(scenarios, 1 = RandomAttack) once
  Reset();
  Run(2.9);
  CHECK(g_calls.empty(), "EnableScenario before the 3 s hold: %zu", g_calls.size());
  CHECK(HasLine("combat_aibots_install", "\"result\":\"installed\"", kInfo) && HasLine("combat_aibots_bots", "\"bots\":3"), "no start / bots records");
  Run(0.3);
  CHECK(g_calls.size() == 1 && g_calls[0].scen == static_cast<void*>(g_scen) && g_calls[0].state == 1 && g_calls[0].at == 103.0, "enable: %zu calls, state %d at %.4f",
        g_calls.size(), g_calls.empty() ? -9 : g_calls[0].state, g_calls.empty() ? 0.0 : g_calls[0].at);
  CHECK(Count("combat_aibots_enable", "\"scenario\":1,\"bots\":3") == 1 && HasLine("combat_aibots_enable", "\"scenario_name\":\"randomattack\"") &&
            HasLine("combat_aibots_enable", "\"case\":\"first call\",\"scenario_object\":\"set\",\"host\":\"yes\",\"combat_map\":0"),
        "no enable record");
  Run(10);
  CHECK(g_calls.size() == 1, "repeated: %zu", g_calls.size());
}

void NoBots() {  // 2
  Reset();
  SetCount(0);
  Run(20);
  CHECK(g_calls.empty() && !HasLine("combat_aibots_bots"), "enable for zero bots");
}

void NotInGameNotCombat() {  // 3
  Reset();
  SetState(8);
  Run(10);
  CHECK(g_calls.empty(), "enable while not in game");
  SetState(9);
  Put<std::uint8_t>(g_net, 0x6341b, 0);
  Run(10);
  CHECK(g_calls.empty(), "enable outside a combat match");
  Put<std::uint8_t>(g_net, 0x6341b, 1);
  Put<std::uintptr_t>(g_net, 0x2ab0, 0);
  Run(10);
  CHECK(g_calls.empty(), "enable without a scenarios object");
  Put<std::uintptr_t>(g_net, 0x2ab0, reinterpret_cast<std::uintptr_t>(g_scen));
  Put<std::uintptr_t>(g_net, 8, 0x1234);  // the net game does not point back at this game
  Run(10);
  CHECK(g_calls.empty(), "enable for a net game of another game");
  Put<std::uintptr_t>(g_net, 8, reinterpret_cast<std::uintptr_t>(g_game));
  Run(4);
  CHECK(g_calls.size() == 1, "not enabled once everything held: %zu", g_calls.size());
}

void TheIdleLine() {  // 3b: in a combat match for 10 s with nothing to do: one line says why, once per match; none when the call was made, none in an arena match
  Reset();
  SetCount(0);  // a combat match (the flag is set) with no bots
  Run(9);
  CHECK(!HasLine("combat_aibots_idle"), "idle line before 10 s");
  Run(3);
  CHECK(Count("combat_aibots_idle", "\"matchiscombat\":1,\"scenarios_object\":\"yes\"") == 1 && Count("combat_aibots_idle", "", kInfo) == 1, "no idle line after 10 s");
  Run(10);
  CHECK(Count("combat_aibots_idle") == 1, "idle line repeated");
  SetState(10);
  Run(1);
  SetState(9);
  Run(9);
  CHECK(Count("combat_aibots_idle") == 1, "idle line for the next match before its 10 s");
  Run(3);
  CHECK(Count("combat_aibots_idle") == 2, "idle line not re-armed for the next match");
  Reset();
  Run(14);
  CHECK(!HasLine("combat_aibots_idle"), "idle line after a successful call");
  // an arena match (the game's combat flag is 0) never writes it, however long it lasts
  Reset();
  Put<std::uint8_t>(g_net, 0x6341b, 0);
  Run(40);
  CHECK(!HasLine("combat_aibots_idle"), "an idle line in a match that is not a combat match");
}

void TheCountAndTheLook() {  // 4, 4b, 5
  Reset();
  Run(2);
  SetCount(4);
  Run(2.9);
  CHECK(g_calls.empty(), "enable although the count changed 2.9 s ago");
  Run(0.5);
  CHECK(g_calls.size() == 1 && g_calls[0].at == 105.0, "enable at %.4f, wanted 105.0", g_calls.empty() ? 0.0 : g_calls[0].at);
  Reset();
  SetCount(0);
  Run(1.0625);
  SetCount(2);
  Run(5);
  CHECK(g_calls.size() == 1 && g_calls[0].at >= 104.0 && g_calls[0].at <= 104.5, "looked at too slowly: enable at %.4f, wanted 104.25", g_calls.empty() ? 0.0 : g_calls[0].at);
  Reset();
  Run(4);
  SetCount(5);
  Run(2.9);
  CHECK(g_calls.size() == 1, "re-enabled too early: %zu", g_calls.size());
  Run(0.5);
  CHECK(g_calls.size() == 2 && HasLine("combat_aibots_enable", "\"case\":\"bot count changed\",\"scenario_object\":\"set\""), "no second call for the new count: %zu", g_calls.size());
  Reset();
  Put<std::uintptr_t>(g_scen, 0x560, 0x99);
  Run(4);
  CHECK(HasLine("combat_aibots_enable", "replacing one already set"), "an existing scenario not reported");
  Reset();
  g_enableSets = false;
  Run(4);
  CHECK(HasLine("combat_aibots_enable", "\"scenario_object\":\"not_set\""), "a scenario that was not created not reported");
}

void LeavingTheMatchRearms() {  // 6
  Reset();
  Run(4);
  CHECK(g_calls.size() == 1, "setup");
  SetState(10);
  Run(1);
  CHECK(Count("combat_aibots_rearmed", "", kInfo) == 1, "no re-armed record");
  Run(2);
  CHECK(Count("combat_aibots_rearmed") == 1, "re-armed repeated");
  SetState(9);
  const double tBack = g_now;
  Run(4);
  CHECK(g_calls.size() == 2 && g_calls[1].at >= tBack + 3.0, "not enabled (or enabled without the hold) in the next match: %zu calls, at %.4f, back at %.4f", g_calls.size(),
        g_calls.size() < 2 ? 0.0 : g_calls[1].at, tBack);
  Reset();
  SetState(10);
  Run(2);
  CHECK(!HasLine("combat_aibots_rearmed"), "re-armed without a call");
  Reset();
  Run(4);
  g_game[0x8510 / 8] = 0;  // no net game at all (between matches)
  Run(1);
  CHECK(Count("combat_aibots_rearmed") == 1, "a vanished net game did not re-arm");
}

void TheRequestFile() {  // 7: scenario and delay, re-read when it changes; off; unknown names keep the old value (a development build)
  Reset(true);
  WriteReq("scenario=off\n");
  Run(10);
  CHECK(g_calls.empty() && Count("combat_aibots_req") == 1, "scenario=off: the request must be read once per change, not per poll (%d)", Count("combat_aibots_req"));
  WriteReq("scenario=aionevone delay=1\n");
  Run(2);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 5, "scenario=aionevone delay=1: %zu calls, state %d", g_calls.size(), g_calls.empty() ? -9 : g_calls[0].state);
  CHECK(g_calls.size() == 1 && g_calls[0].at - 110.0 < 2.0, "delay=1 not honoured (at %.4f)", g_calls.empty() ? 0.0 : g_calls[0].at);
  WriteReq("scenario=4\n");
  Run(5);
  CHECK(g_calls.size() == 1, "a changed request re-enabled by itself: %zu", g_calls.size());
  Reset(true, nullptr, true, "scenario=3 delay=0.5 debug=1\n");  // present when the module installs
  Run(2);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 3 && g_calls[0].at == 100.5, "request present at start: state %d at %.4f", g_calls.empty() ? -9 : g_calls[0].state,
        g_calls.empty() ? 0.0 : g_calls[0].at);
  Reset(true);
  WriteReq("scenario=PATROL\n");
  Run(4);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 0, "scenario=PATROL");
  Reset(true);
  WriteReq("scenario=bogus\n");
  Run(4);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 1 && HasLine("combat_aibots_scenario", "", kWarn), "unknown scenario name");
  Reset(true);
  WriteReq("scenario=7\n");
  Run(4);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 1 && HasLine("combat_aibots_scenario", "", kWarn), "scenario 7 out of range");
}

void TooManyAgents() {  // 8
  Reset();
  SetCount(15);
  Run(4);
  CHECK(HasLine("combat_aibots_agents", "", kWarn), "no warning for 15 agents");
  Reset();
  SetCount(14);
  Run(4);
  CHECK(!HasLine("combat_aibots_agents"), "warning for 14 agents");
}

void TheEnableRecordContext() {  // 9
  const std::uint64_t flagCases[][2] = {{2, 1}, {0x40, 1}, {0x44, 0}, {0, 0}, {0x4, 0}};
  for (const auto& fc : flagCases) {
    Reset();
    g_flagsPtr[0] = fc[0];
    Run(4);
    CHECK(HasLine("combat_aibots_enable", fc[1] ? "\"host\":\"yes\"" : "\"host\":\"no\""), "flags %llx: host not %s", static_cast<unsigned long long>(fc[0]), fc[1] ? "yes" : "no");
  }
  Reset();
  SetLevels({0x43e2da7914642604ull});
  Run(4);
  CHECK(HasLine("combat_aibots_enable", "\"combat_map\":1"), "combat map not reported");
}

void ThePinnedAddress() {  // 10
  ai::Fns f{};
  const char* why = "";
  g_word[0] = 0xd100c3ff;
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&g_word[0]) - 0x123c2bc;
  g_lookupRet = nullptr;
  CHECK(ai::Resolve(base, nullptr, &f, &why) && f.named == 0 && reinterpret_cast<std::uintptr_t>(f.enable) == base + 0x123c2bc, "fixed-address resolve");
  g_lookupRet = &g_word[0];
  CHECK(ai::Resolve(base, &Lookup, &f, &why) && f.named == 1, "name at the pinned address");
  g_lookupRet = &g_word[1];
  ai::Fns g{};
  CHECK(!ai::Resolve(base, &Lookup, &g, &why) && g.enable == nullptr, "a name at another address accepted");
  g_word[0] = 0xd100c3fe;
  CHECK(!ai::Resolve(base, nullptr, &g, &why) && g.enable == nullptr && std::strstr(why, "first word") != nullptr, "another first word accepted");
  CHECK(!ai::Resolve(kBase, nullptr, &g, &why) && std::strstr(why, "unreadable") != nullptr, "an unreadable address accepted");
}

void NoLibrary() {  // 11
  g_lines.clear();
  nevr_quest::combat::tick::ResetForTest();
  ai::ResetForTest();
  ai::SetClockForTest(&Clock);
  ai::Switches sw;
  sw.on = true;
  const nevr_quest::combat::ModuleResult r = ai::InstallAt([](const char*, sentinel::ElfImage*) { return false; }, nullptr, sw);
  BuildWorld();
  g_calls.clear();
  Run(5);
  CHECK(!r.installed && g_calls.empty() && ai::Phase() == 2 && nevr_quest::combat::tick::Count() == 0 && HasLine("combat_aibots_install", "\"reason\":\"module_not_loaded\"", kError),
        "missing library not loud");
}

void TheSwitch() {  // 12
  const nevr_quest::combat::ModuleResult r = Reset(false, nullptr, /*on=*/false);
  (void)r;
  g_lines.clear();
  ai::Switches sw;
  sw.on = false;
  nevr_quest::combat::tick::ResetForTest();
  ai::ResetForTest();
  const nevr_quest::combat::ModuleResult off = ai::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, nullptr, sw);
  CHECK(!off.installed && off.off && std::strcmp(off.status, "off_config") == 0 && ai::Phase() == 2 && nevr_quest::combat::tick::Count() == 0, "off result");
  CHECK(g_lines.size() == 1 && HasLine("combat_aibots_install", "\"result\":\"off\",\"reason\":\"off_config\",\"switch\":\"combat.aibots\"", kInfo), "off line");
  BuildWorld();
  Run(10);
  CHECK(ai::CurrentStats().looks == 0 && g_calls.empty(), "an off module looked at the game");
}

void TheScenarioKey() {  // 13
  struct Case {
    const char* text;
    int state;
    bool warn;
  };
  const Case cases[] = {{nullptr, 1, false}, {"randomattack", 1, false}, {"Patrol", 0, false}, {"6", 6, false}, {"aibdebugarena", 6, false}, {"spike", 3, false},
                        {"bogus", 1, true},  {"7", 1, true},            {"", 1, true}};
  for (const Case& c : cases) {
    Reset(false, c.text);
    Run(4);
    CHECK(g_calls.size() == 1 && g_calls[0].state == c.state, "scenario '%s': %zu calls, state %d", c.text != nullptr ? c.text : "(null)", g_calls.size(),
          g_calls.empty() ? -9 : g_calls[0].state);
    CHECK(HasLine("combat_aibots_scenario", "", kWarn) == c.warn, "scenario '%s': warning %d", c.text != nullptr ? c.text : "(null)", c.warn ? 1 : 0);
    CHECK(c.text == nullptr || std::strcmp(c.text, "bogus") != 0 || !HasLine("combat_aibots_scenario", "bogus"), "the rejected word was logged");
  }
  // off in a release build is the module off; in a development build the request file can still turn it on.
  const nevr_quest::combat::ModuleResult r = Reset(false, "off");
  CHECK(!r.installed && r.off && ai::Phase() == 2 && nevr_quest::combat::tick::Count() == 0 && HasLine("combat_aibots_install", "\"reason\":\"scenario_off\""), "scenario off, release");
  Run(10);
  CHECK(g_calls.empty(), "scenario off still called");
  const nevr_quest::combat::ModuleResult dev = Reset(true, "off");
  CHECK(dev.installed && ai::Phase() == 1, "scenario off, development build");
  WriteReq("scenario=2\n");
  Run(5);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 2, "the request file did not turn a development build's scenario on");
}

void AReleaseBuildReadsNoRequestFile() {  // 14
  ClearDir();
  WriteReq("scenario=3 delay=0.5\n");
  g_lines.clear();
  ai::Switches sw;
  sw.on = true;
  nevr_quest::combat::tick::ResetForTest();
  ai::ResetForTest();
  ai::SetClockForTest(&Clock);
  ai::SetLegacyDirForTest(g_dir.c_str());
  ai::InstallAt([](const char*, sentinel::ElfImage*) { return false; }, nullptr, sw);  // refused here: the warning comes first
  const std::vector<Line> legacy = Lines("combat_aibots_legacy_switch");
  CHECK(legacy.size() == 1 && legacy[0].level == sentinel::LogLevel::kWarn && Has(legacy[0].text, "\"file\":\"aibots.req\"") &&
            Has(legacy[0].text, "\"replaced_by\":\"combat.aibots_scenario\""),
        "no legacy warning");
  Reset(false, nullptr, true, "scenario=3 delay=0.5\n");
  Run(10);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 1 && g_calls[0].at == 103.0 && !HasLine("combat_aibots_req"), "a release build read the request file");
  g_lines.clear();
  ai::Switches dev;
  dev.on = true;
  dev.devBuild = true;
  nevr_quest::combat::tick::ResetForTest();
  ai::ResetForTest();
  ai::SetLegacyDirForTest(g_dir.c_str());
  ai::InstallAt([](const char*, sentinel::ElfImage*) { return false; }, nullptr, dev);
  CHECK(!HasLine("combat_aibots_legacy_switch"), "a development build warned about a file it reads");
}

void AnEchoArenaLevelIsLeftAlone() {  // 15
  const std::uint64_t arena[] = {CSymbol64Hash("mpl_arena_a"), CSymbol64Hash("mpl_arena_a_lowspec"), CSymbol64Hash("mpl_arena_b"), CSymbol64Hash("mpl_tutorial_a")};
  for (const std::uint64_t level : arena) {
    Reset();
    SetLevels({level});
    Run(10);
    CHECK(g_calls.empty() && ai::CurrentStats().enables == 0 && ai::CurrentStats().scopedOut > 0, "an Echo Arena level: %zu calls, %u scoped out", g_calls.size(),
          ai::CurrentStats().scopedOut);
    CHECK(Count("combat_aibots_scoped_out") == 1 && Count("combat_aibots_scoped_out", "", kInfo) == 1, "scoped out %d times", Count("combat_aibots_scoped_out"));
  }
  // a transition: the arena level is still loaded beside a combat map, and wins
  Reset();
  SetLevels({CSymbol64Hash("mpl_combat_dyson"), CSymbol64Hash("mpl_arena_a")});
  Run(10);
  CHECK(g_calls.empty(), "a transition holding an arena level called EnableScenario");
  // the arena level goes: the call is made
  SetLevels({CSymbol64Hash("mpl_combat_dyson")});
  Run(1);
  CHECK(g_calls.size() == 1, "not called once the arena level left");
  // a later match in an arena level says it again (one line per match)
  SetState(10);
  Run(1);
  SetState(9);
  SetLevels({CSymbol64Hash("mpl_arena_a")});
  Run(5);
  CHECK(Count("combat_aibots_scoped_out") == 2 && g_calls.size() == 1, "the second match: %d scoped-out lines, %zu calls", Count("combat_aibots_scoped_out"), g_calls.size());
  SetState(10);
  Run(1);
  SetState(9);
  Run(5);
  CHECK(Count("combat_aibots_scoped_out") == 3, "the scoped-out line was not said again for the next match (%d)", Count("combat_aibots_scoped_out"));
  // the combat lobby and the lobbies arena players use are not Echo Arena levels: the game's own flags decide, as before
  Reset();
  SetLevels({CSymbol64Hash("mpl_lobby_b_combat")});
  Run(4);
  CHECK(g_calls.size() == 1, "the combat lobby stopped the call");
}

// By count: an arena match (the game's own combat flag is 0, no bots) costs the look's four reads and nothing else: no level scan, no read of the scenarios.
void AnArenaMatchIsFourReadsALookAndNoScan() {
  for (const char* name : {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a"}) {
    Reset();
    SetLevels({CSymbol64Hash(name)});
    Put<std::uint8_t>(g_net, 0x6341b, 0);
    SetCount(0);
    const std::uint32_t reads0 = nevr_quest::combat::SafeReadCalls(), scans0 = nevr_quest::combat::level::ScanCalls();
    Run(30);  // well past the 10 s the idle line waits for
    const std::uint32_t looks = ai::CurrentStats().looks;
    CHECK(looks >= 115 && g_calls.empty() && !HasLine("combat_aibots_idle"), "%s: %u looks, %zu calls, an idle line %d", name, looks, g_calls.size(), HasLine("combat_aibots_idle") ? 1 : 0);
    CHECK(nevr_quest::combat::level::ScanCalls() == scans0, "%s: %u level scans in an arena match", name, nevr_quest::combat::level::ScanCalls() - scans0);
    CHECK(nevr_quest::combat::SafeReadCalls() - reads0 == looks * 4, "%s: %u reads for %u looks, wanted 4 each", name, nevr_quest::combat::SafeReadCalls() - reads0, looks);
  }
  // No net game at all (between matches): one read a look.
  Reset();
  g_game[0x8510 / 8] = 0;
  const std::uint32_t reads1 = nevr_quest::combat::SafeReadCalls();
  Run(4);
  CHECK(nevr_quest::combat::SafeReadCalls() - reads1 == ai::CurrentStats().looks, "no net game: %u reads for %u looks", nevr_quest::combat::SafeReadCalls() - reads1, ai::CurrentStats().looks);
}

void TheInstallOnAnImage() {  // 16
  combat_test::Image image;
  static combat_test::Image* sImage;
  sImage = &image;
  auto lookup = [](const char* module, sentinel::ElfImage* out) {
    if (!sImage->Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
    *out = sImage->Elf();
    return true;
  };
  const nevr_quest::combat::pinned::Pins pins[] = {at::kPins, nevr_quest::combat::netgame::kPins};
  auto put = [&] { combat_test::PutPins(image, pins, 2); };
  auto begin = [&] {
    g_lines.clear();
    nevr_quest::combat::tick::ResetForTest();
    ai::ResetForTest();
    ai::SetLegacyDirForTest(g_dir.c_str());
  };
  ai::Switches sw;
  sw.on = true;
  image.Build({});
  put();
  begin();
  nevr_quest::combat::ModuleResult r = ai::InstallAt(lookup, nullptr, sw);
  CHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off && ai::Phase() == 1 && nevr_quest::combat::tick::Count() == 1, "install on the pinned image");
  const std::size_t bent = combat_test::ForEachBentPin(image, pins, 2, [&](const char* what) {
    begin();
    r = ai::InstallAt(lookup, nullptr, sw);
    CHECK(!r.installed && !r.off && std::strcmp(r.status, "pinned_word_mismatch") == 0 && nevr_quest::combat::tick::Count() == 0 && HasLine("combat_aibots_pinned_word", what),
          "pin %s: %s", what, r.status);
  });
  CHECK(bent >= 15, "only %zu pins were bent", bent);
  image.Build({}, "0123456789abcdef0123456789abcdef01234567");
  put();
  begin();
  r = ai::InstallAt(lookup, nullptr, sw);
  CHECK(!r.installed && std::strcmp(r.status, "build_id_mismatch") == 0 && nevr_quest::combat::tick::Count() == 0, "another build id installed");
  image.Build({});
  put();
  begin();
  g_lookupRet = reinterpret_cast<void*>(image.Base() + 0x1000);
  r = ai::InstallAt(lookup, &Lookup, sw);
  CHECK(!r.installed && std::strcmp(r.status, "export_elsewhere") == 0, "a name elsewhere installed");
  begin();
  for (std::size_t i = 0; i < nevr_quest::combat::tick::kMaxCallbacks; ++i) nevr_quest::combat::tick::Add([](std::uintptr_t) noexcept {});
  r = ai::InstallAt(lookup, nullptr, sw);
  CHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0 && ai::Phase() == 2, "a full tick table installed");
  nevr_quest::combat::tick::ResetForTest();
  image.Drop();
}

double Seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

void TheCostOfALook() {  // 17: printed, not asserted (a host number; the module's header says so)
  Reset();
  SetLevels({CSymbol64Hash("mpl_arena_a")});
  Put<std::uint8_t>(g_net, 0x6341b, 0);  // an arena match: no combat flag
  SetCount(0);
  const int looks = 100000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < looks; ++i) {
    g_now += 1.0;
    ai::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  }
  const auto t1 = std::chrono::steady_clock::now();
  const int frames = 2000000;
  for (int i = 0; i < frames; ++i) ai::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  const auto t2 = std::chrono::steady_clock::now();
  std::printf("aibots_test: arena look %.2f us (4 reads, 4 Hz), frame with no look due %.1f ns (x86 host)\n", Seconds(t0, t1) / looks * 1e6, Seconds(t1, t2) / frames * 1e9);
  CHECK(ai::CurrentStats().looks >= static_cast<std::uint32_t>(looks) && ai::CurrentStats().enables == 0, "the looks");
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/combat-ai-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  sentinel::SetLogSink(&Capture);
  TheDefaults();
  NoBots();
  NotInGameNotCombat();
  TheIdleLine();
  TheCountAndTheLook();
  LeavingTheMatchRearms();
  TheRequestFile();
  TooManyAgents();
  TheEnableRecordContext();
  ThePinnedAddress();
  NoLibrary();
  TheSwitch();
  TheScenarioKey();
  AReleaseBuildReadsNoRequestFile();
  AnEchoArenaLevelIsLeftAlone();
  AnArenaMatchIsFourReadsALookAndNoScan();
  TheInstallOnAnImage();
  TheCostOfALook();
  ClearDir();
  rmdir(g_dir.c_str());
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "aibots_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("aibots_test: all checks passed\n");
  return 0;
}
