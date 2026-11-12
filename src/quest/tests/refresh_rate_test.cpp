// Host test for the combat refresh-rate rule (#2172, #2000, #1648; docs/adr/0046 rule 3, docs/adr/0039): src/quest/combat/refresh_rate.cpp,
// built without exceptions as the sentinel builds it. The rule moved here from the q15t plugin (refresh_rate.cpp), whose host test cases
// are these: the level rate (90, 72 on a Quest 2), a combat or lobby level asks for it, an Echo Arena level keeps the session rate, a
// request above the level rate while a level is loaded is kept at it, the tick re-asks on a change of level only, and the three keys
// switch the parts. What is new is the owner's shape: with `combat::Active()` false the answer is the game's own rate and nothing is
// logged or registered (the release case), and the tick's re-ask goes through the function now in the slot, which the render-configuration
// handler (render_config_hook_test.cpp) sees and does not take for a request of the game's.
//
// The game is a model: CR15Game's two gamespace tables and a gamespace's level, laid out where the rule reads them (the offsets are
// spelled in this file, not taken from the header), read by process_vm_readv as in production.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/refresh_rate.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "nevr/symbol_hash.h"
#include "pinned_targets.h"
#include "quest/combat/combat_active.h"
#include "quest/combat/combat_install.h"
#include "quest/combat/combat_tick.h"
#include "quest/headset_active.h"
#include "quest/headset_profile.h"
#include "quest/tests/test_check.h"

// sendguard's table row reads the sentinel's files folder (the plugin's old switch file); the test names a folder that does not exist.
namespace sentinel {
const char* FilesDir() { return "/nonexistent-nevr-files"; }
}  // namespace sentinel

namespace {

namespace bv = nevr_quest::build_variant;
namespace combat = nevr_quest::combat;
namespace rr = nevr_quest::combat::refresh_rate;
using EchoVR::CSymbol64Hash;

// ---- the log ------------------------------------------------------------------------------------------------------------------

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

// ---- the game, as this test spells it -----------------------------------------------------------------------------------------

constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kTable1 = 0x1f88, kCount1 = 0x1fb8;  // CR15Game
constexpr std::size_t kRowSize = 16, kRowGamespace = 8;                                        // a table row {sym, gs*}
constexpr std::size_t kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;                            // a gamespace

struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  std::uintptr_t table1[8][2] = {};
  Gamespace spaces[8];
};
Game g_game;

std::uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(g_game.words); }

void ClearGame() { g_game = Game{}; }

// Puts a gamespace holding `level` in table `table` (0 or 1) at row `row`, owned by the game unless `owner` says another.
void PutLevel(int table, int row, std::uint64_t level, std::uintptr_t owner = 0) {
  Gamespace& gs = g_game.spaces[table * 4 + row];
  gs.words[kGamespaceOwner / 8] = owner != 0 ? owner : GameAddress();
  gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(level);
  std::uintptr_t(*rows)[2] = table == 0 ? g_game.table0 : g_game.table1;
  rows[row][0] = 0x5555;
  rows[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
  g_game.words[(table == 0 ? kTable0 : kTable1) / 8] = reinterpret_cast<std::uintptr_t>(rows);
  std::uintptr_t& count = g_game.words[(table == 0 ? kCount0 : kCount1) / 8];
  if (static_cast<std::uintptr_t>(row) + 1 > count) count = static_cast<std::uintptr_t>(row) + 1;
}

// ---- the world the rule runs in ------------------------------------------------------------------------------------------------

struct Ask {
  void* ovr;
  float hz;
};
std::vector<Ask> g_asks;
int g_askResult = 0;
bool g_askThroughHandler = true;  // the re-ask goes through the render-configuration handler: Choose, then Note

double g_now = 100.0;
double Clock() noexcept { return g_now; }

int FakeAsk(void* ovr, float hz) noexcept {
  g_asks.push_back({ovr, hz});
  if (g_askThroughHandler) {  // what the render-configuration handler does with a call that reaches it
    const float sent = rr::Choose(ovr, hz);
    rr::Note(hz, sent, g_askResult);
  }
  return g_askResult;
}

void* const kOvr = reinterpret_cast<void*>(0x1234);

nevr_quest::CombatConfig g_cfg;
combat::ModuleResult InstallRule() { return rr::Install(g_cfg.refreshRate.on, g_cfg.refreshRateLobby.on, g_cfg.refreshRateQuest2At72.on); }

// A process of this variant, the rule's seams at the test's, and the keys as given.
void World(bv::Variant variant, const nevr_quest::CombatConfig& config = nevr_quest::CombatConfig{}, int quest2 = 0) {
  combat::tick::ResetForTest();
  combat::ResetForTest();
  combat::ResetInstallForTest();
  rr::ResetForTest();
  g_lines.clear();
  g_asks.clear();
  g_askResult = 0;
  g_askThroughHandler = true;
  g_now = 100.0;
  ClearGame();
  combat::SetVariant(variant);
  sentinel::SetLogSink(&Capture);
  combat::Resolve(/*devBuild=*/false, /*forceAsked=*/false);
  combat::SetConfig(config);
  g_cfg = config;
  rr::SetAskForTest(&FakeAsk);
  // The headset is the identity step's profile (quest/headset_active.h): a Quest 2, or a Quest 3 for every other case here.
  nevr_quest::headset_active::SetActive(quest2 != 0 ? &nevr_quest::headset_profile::kQuest2 : &nevr_quest::headset_profile::kQuest3);
  rr::SetClockForTest(&Clock);
  g_lines.clear();  // the combat_active line is combat_active_test's
}
void Done() {
  rr::SetAskForTest(nullptr);
  nevr_quest::headset_active::SetActive(nullptr);
  rr::SetClockForTest(nullptr);
  combat::tick::ResetForTest();
  sentinel::SetLogSink(nullptr);
}

nevr_quest::CombatConfig Keys(bool rule, bool lobby, bool quest2At72) {
  nevr_quest::CombatConfig c;
  c.refreshRate.on = rule;
  c.refreshRateLobby.on = lobby;
  c.refreshRateQuest2At72.on = quest2At72;
  return c;
}

// The game asks, through the render-configuration handler: what is sent comes back.
float GameAsks(float hz) {
  const float sent = rr::Choose(kOvr, hz);
  rr::Note(hz, sent, 0);
  return sent;
}
// One frame of the tick, `seconds` after the last look.
void Frame(double seconds = 1.0) {
  g_now += seconds;
  rr::Tick(GameAddress());
}

// ---- the pure rule ------------------------------------------------------------------------------------------------------------

void TheRuleFollowsItsThreeKeysAndTheHeadset() {
  struct Row {
    bool rule, lobby, at72, quest2;
    bool wantOn, wantLobby;
    float wantHz;
  };
  const Row rows[] = {
      {true, true, true, false, true, true, 90.0f},     // a Quest 3, everything on: 90, the lobby too
      {true, true, true, true, true, true, 72.0f},      // a Quest 2: 72
      {true, true, false, true, true, true, 90.0f},     // refresh_rate_quest2_72 off: a Quest 2 asks 90 too
      {true, false, true, false, true, false, 90.0f},   // refresh_rate_lobby off: combat maps only
      {true, false, true, true, true, false, 72.0f},
      {false, true, true, false, false, false, 90.0f},  // refresh_rate off: the game's rate always (witness)
      {false, true, true, true, false, false, 90.0f},   // off, so no 72 either
  };
  for (const Row& r : rows) {
    rr::Inputs in;
    in.rule = r.rule;
    in.lobby = r.lobby;
    in.quest2At72 = r.at72;
    in.quest2 = r.quest2;
    const rr::Rule rule = rr::Decide(in);
    QCHECK(rule.on == r.wantOn);
    QCHECK(rule.lobby == r.wantLobby);
    QCHECK(rule.levelHz == r.wantHz);
  }
  QCHECK(rr::kLevelHz == 90.0f && rr::kQuest2LevelHz == 72.0f);
}

// The headset is the identity step's profile (quest/headset_active.h, ADR 0031 decision 4, #2037): the Quest 2 profile asks 72, the Quest 3
// and the generic profile ask the match rate. The rule reads no system property of its own. With no profile set (a sequence that did not
// run the step), the generic profile's answer, never the Quest 2's, and one Warning that says so.
void TheHeadsetIsTheIdentityStepsProfile() {
  struct Row {
    const nevr_quest::headset_profile::Profile* profile;
    const char* quest2;
    const char* hz;
  };
  const Row rows[] = {{&nevr_quest::headset_profile::kQuest2, "\"quest2\":1", "\"level_hz\":\"72\""},
                      {&nevr_quest::headset_profile::kQuest3, "\"quest2\":0", "\"level_hz\":\"90\""},
                      {&nevr_quest::headset_profile::kGeneric, "\"quest2\":0", "\"level_hz\":\"90\""},
                      {nullptr, "\"quest2\":0", "\"level_hz\":\"90\""}};
  for (const Row& r : rows) {
    World(bv::Variant::kCombat, Keys(true, true, true));
    nevr_quest::headset_active::SetActive(r.profile);
    QCHECK(InstallRule().installed);
    const std::vector<Line> l = Lines("combat_refresh_rate_install");
    QCHECK(l.size() == 1U && Has(l[0].text, r.quest2) && Has(l[0].text, r.hz));
    const std::vector<Line> missing = Lines("combat_refresh_rate_profile_missing");
    if (r.profile != nullptr) {
      QCHECK(missing.empty());
    } else {
      QCHECK(missing.size() == 1U);
      if (missing.size() == 1U) {
        QCHECK(missing[0].level == sentinel::LogLevel::kWarn);
        const std::size_t at = missing[0].text.find("\"event\"");
        QCHECK(at != std::string::npos &&
               missing[0].text.substr(at) ==
                   "\"event\":\"combat_refresh_rate_profile_missing\",\"profile\":\"generic\",\"level_hz\":\"90\","
                   "\"consequence\":\"the identity step set no headset profile before this step, so the rule asks the generic profile's level rate\"}");
      }
    }
    Done();
  }
  // The rule off (witness) and no profile: still one Warning, but its consequence is the true one for that mode, which asks no level rate
  // (no tick is registered, so no switch line is written); the `quest2` field of the install line and of every request line, which the
  // witness still writes, reads the generic profile's 0. The Warning carries no `level_hz` either.
  World(bv::Variant::kCombat, Keys(false, true, true));
  nevr_quest::headset_active::SetActive(nullptr);
  QCHECK(InstallRule().installed);
  const std::vector<Line> witnessInstall = Lines("combat_refresh_rate_install");
  QCHECK(witnessInstall.size() == 1U && Has(witnessInstall[0].text, "\"mode\":\"witness\"") && Has(witnessInstall[0].text, "\"quest2\":0"));
  const std::vector<Line> witnessMissing = Lines("combat_refresh_rate_profile_missing");
  QCHECK(witnessMissing.size() == 1U);
  if (witnessMissing.size() == 1U) {
    QCHECK(witnessMissing[0].level == sentinel::LogLevel::kWarn);
    const std::size_t at = witnessMissing[0].text.find("\"event\"");
    QCHECK(at != std::string::npos &&
           witnessMissing[0].text.substr(at) ==
               "\"event\":\"combat_refresh_rate_profile_missing\",\"profile\":\"generic\","
               "\"consequence\":\"the identity step set no headset profile before this step; refresh_rate is off, so the rule asks no level "
               "rate; the quest2 field of the install line and of each request line reads the generic profile's 0\"}");
  }
  // The game asks above the level rate and a combat level is loaded: with the rule on, the next frame would re-ask 90 and write a switch
  // line. With it off the request is logged as the game made it, and the frame re-asks nothing and writes no switch line: this is what pins
  // the tick's `g_on` gate (a session rate at or below the level rate, or no level, would stop the tick before the gate mattered).
  QCHECK(GameAsks(120.0f) == 120.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  Frame();
  QCHECK(g_asks.empty());
  const std::vector<Line> witnessRequest = Lines("combat_refresh_rate_request");
  QCHECK(witnessRequest.size() == 1U && Has(witnessRequest[0].text, "\"mode\":\"witness\"") && Has(witnessRequest[0].text, "\"quest2\":0"));
  QCHECK(Lines("combat_refresh_rate_switch").empty());
  Done();
  // In a release APK the rule does not run, so it reads no profile and says nothing about one.
  World(bv::Variant::kNone);
  nevr_quest::headset_active::SetActive(nullptr);
  QCHECK(!InstallRule().installed);
  QCHECK(Lines("combat_refresh_rate_profile_missing").empty());
  Done();
}

void ARequestAboveTheLevelRateInALevelIsKept() {
  rr::Rule on;
  on.on = true;
  on.lobby = true;
  on.levelHz = 90.0f;
  QCHECK(rr::RateFor(on, true, 120.0f) == 90.0f);   // above, in a level: kept at the level rate
  QCHECK(rr::RateFor(on, true, 90.0f) == 90.0f);    // at it: as asked
  QCHECK(rr::RateFor(on, true, 72.0f) == 72.0f);    // below it: never raised
  QCHECK(rr::RateFor(on, false, 120.0f) == 120.0f); // no level: as asked
  rr::Rule off = on;
  off.on = false;
  QCHECK(rr::RateFor(off, true, 120.0f) == 120.0f); // the rule off: as asked
  rr::Rule q2 = on;
  q2.levelHz = 72.0f;
  QCHECK(rr::RateFor(q2, true, 90.0f) == 72.0f);
}

// ---- the owner, release and variants ------------------------------------------------------------------------------------------

// The release case: combat does not run here, so the answer is the game's own rate whatever is loaded and nothing is logged, kept or
// registered.
void ThePlainReleaseHasNoOwnerAndTheGamesRateIsUntouched() {
  for (const bv::Variant variant : {bv::Variant::kNone, bv::Variant::kPerfProbe}) {
    World(variant);
    PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
    QCHECK(!combat::Active());
    QCHECK(rr::Choose(kOvr, 120.0f) == 120.0f);
    QCHECK(rr::Choose(kOvr, 72.0f) == 72.0f);
    rr::Note(120.0f, 120.0f, 0);
    QCHECK(Lines("combat_refresh_rate_request").empty());
    QCHECK(rr::CurrentStats().requests == 0);
    // Nothing else changed: no request kept, no level state, no switch, and the tick's table is empty.
    const rr::Stats stats = rr::CurrentStats();
    QCHECK(stats.switches == 0 && stats.lastAsked == -1.0f && stats.lastSent == -1.0f && !stats.inLevel);
    QCHECK(combat::tick::Count() == 0);
    Frame();
    QCHECK(g_asks.empty());  // the tick never asks: the game's rate stays
    // Even a step that is reached anyway installs nothing and registers no tick.
    const combat::ModuleResult r = InstallRule();
    QCHECK(!r.installed);
    QCHECK(combat::tick::Count() == 0);
    QCHECK(Lines("combat_refresh_rate_install").empty());
    Done();
  }
}

// On a q15t or combat APK this module is the one owner: it registers its tick, decides the rate and names every input on its line.
void ACombatVariantApkOwnsTheRuleAndSaysWhatItCombined() {
  for (const bv::Variant variant : {bv::Variant::kQ15t, bv::Variant::kCombat}) {
    World(variant);
    const combat::ModuleResult r = InstallRule();
    QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
    QCHECK(combat::tick::Count() == 1);
    const std::vector<Line> lines = Lines("combat_refresh_rate_install");
    QCHECK(lines.size() == 1U);
    if (lines.size() == 1U) {
      QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
      QCHECK(Has(lines[0].text, "\"mode\":\"match\""));
      QCHECK(Has(lines[0].text, "\"refresh_rate\":1") && Has(lines[0].text, "\"refresh_rate_lobby\":0") &&
             Has(lines[0].text, "\"refresh_rate_quest2_72\":1"));
      QCHECK(Has(lines[0].text, "\"quest2\":0") && Has(lines[0].text, "\"level_hz\":\"90\""));
      QCHECK(Has(lines[0].text, "\"tick\":\"registered\""));
      QCHECK(Has(lines[0].text, "\"msg\":"));
    }
    Done();
  }
}

void TheInstallLineFollowsTheKeysAndTheHeadset() {
  World(bv::Variant::kCombat, Keys(true, true, true), /*quest2=*/1);
  InstallRule();
  std::vector<Line> l = Lines("combat_refresh_rate_install");
  QCHECK(l.size() == 1U && Has(l[0].text, "\"quest2\":1") && Has(l[0].text, "\"level_hz\":\"72\""));
  Done();

  World(bv::Variant::kCombat, Keys(true, true, false), /*quest2=*/1);
  InstallRule();
  l = Lines("combat_refresh_rate_install");
  QCHECK(l.size() == 1U && Has(l[0].text, "\"level_hz\":\"90\"") && Has(l[0].text, "\"refresh_rate_quest2_72\":0"));
  Done();

  World(bv::Variant::kCombat, Keys(true, false, true), 0);
  InstallRule();
  l = Lines("combat_refresh_rate_install");
  QCHECK(l.size() == 1U && Has(l[0].text, "\"mode\":\"match\"") && Has(l[0].text, "\"refresh_rate_lobby\":0"));
  Done();

  // The rule off: the witness only, which registers no tick and still reports it installed.
  World(bv::Variant::kCombat, Keys(false, true, true), 0);
  const combat::ModuleResult r = InstallRule();
  QCHECK(r.installed && std::strcmp(r.status, "witness") == 0);
  QCHECK(combat::tick::Count() == 0);
  l = Lines("combat_refresh_rate_install");
  QCHECK(l.size() == 1U && Has(l[0].text, "\"mode\":\"witness\"") && Has(l[0].text, "\"tick\":\"not_needed\"") &&
         Has(l[0].text, "\"refresh_rate\":0"));
  Done();
}

void AFullTickTableIsNamedAndTheModuleSaysItDidNotInstall() {
  World(bv::Variant::kCombat);
  for (std::size_t i = 0; i < combat::tick::kMaxCallbacks; ++i) {
    QCHECK(combat::tick::Add([](std::uintptr_t) noexcept {}));
  }
  const combat::ModuleResult r = InstallRule();
  QCHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0);
  const std::vector<Line> l = Lines("combat_refresh_rate_install");
  QCHECK(l.size() == 1U && l[0].level == sentinel::LogLevel::kWarn && Has(l[0].text, "\"tick\":\"full\""));
  Done();
}

// ---- a session ----------------------------------------------------------------------------------------------------------------

// Echo Arena (120 asked, nothing loaded): untouched. A combat map loads: the tick re-asks 90 once. The game asks 120 again during it:
// kept at 90. The map unloads: the tick re-asks the session rate once.
void AComSessionFromArenaToCombatAndBack() {
  World(bv::Variant::kCombat, Keys(true, true, true));
  InstallRule();
  g_lines.clear();
  Frame();  // before the game's first request there is nothing to lower
  QCHECK(g_asks.empty());

  QCHECK(GameAsks(120.0f) == 120.0f);  // the first request, with no level loaded
  std::vector<Line> req = Lines("combat_refresh_rate_request");
  QCHECK(req.size() == 1U);
  if (req.size() == 1U) {
    QCHECK(Has(req[0].text, "\"asked_hz\":\"120.0\"") && Has(req[0].text, "\"sent_hz\":\"120.0\"") && Has(req[0].text, "\"n\":1"));
    QCHECK(Has(req[0].text, "\"why\":\"game request\"") && Has(req[0].text, "\"mode\":\"lobby\"") && Has(req[0].text, "\"quest2\":0"));
  }
  Frame();
  QCHECK(g_asks.empty());  // an Echo Arena level or none: the game's rate

  PutLevel(0, 0, CSymbol64Hash("mpl_combat_gauss"));
  Frame(0.1);  // the tick looks every 0.5 s
  QCHECK(g_asks.empty());
  Frame(1.0);
  QCHECK(g_asks.size() == 1U && g_asks[0].hz == 90.0f && g_asks[0].ovr == kOvr);  // entering a combat map asks 90 with the game's own ovr
  std::vector<Line> sw = Lines("combat_refresh_rate_switch");
  QCHECK(sw.size() == 1U);
  if (sw.size() == 1U) {
    QCHECK(sw[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(sw[0].text, "\"asked_hz\":\"120.0\"") && Has(sw[0].text, "\"sent_hz\":\"90.0\"") && Has(sw[0].text, "\"n\":1"));
    char level[40];
    std::snprintf(level, sizeof level, "\"level\":\"%016llx\"", static_cast<unsigned long long>(CSymbol64Hash("mpl_combat_gauss")));
    QCHECK(Has(sw[0].text, level));
    QCHECK(Has(sw[0].text, "\"why\":\"combat or lobby level loaded\"") && Has(sw[0].text, "\"mode\":\"lobby\""));
  }
  // The re-ask passed through the handler (FakeAsk calls Choose and Note) and was not taken for a request of the game's.
  QCHECK(rr::CurrentStats().requests == 1);
  QCHECK(Lines("combat_refresh_rate_request").size() == 1U);

  Frame();
  QCHECK(g_asks.size() == 1U);  // no change, no re-ask

  QCHECK(GameAsks(120.0f) == 90.0f);  // the game asks again during the match: kept at the level rate
  req = Lines("combat_refresh_rate_request");
  QCHECK(req.size() == 2U);
  if (req.size() == 2U) {
    QCHECK(Has(req[1].text, "\"asked_hz\":\"120.0\"") && Has(req[1].text, "\"sent_hz\":\"90.0\""));
    QCHECK(Has(req[1].text, "kept at the level rate"));
  }
  QCHECK(rr::CurrentStats().requests == 2);

  // A lobby level after a combat map is still the level rate: no re-ask.
  PutLevel(0, 0, CSymbol64Hash("mpl_lobby_b_combat"));
  Frame();
  QCHECK(g_asks.size() == 1U);
  // An Echo Arena level loaded beside it wins: the session rate comes back.
  PutLevel(1, 0, CSymbol64Hash("mpl_arena_a"));
  Frame();
  QCHECK(g_asks.size() == 2U && g_asks[1].hz == 120.0f);
  sw = Lines("combat_refresh_rate_switch");
  QCHECK(sw.size() == 2U && Has(sw[1].text, "echo arena level loaded: session rate") && Has(sw[1].text, "\"sent_hz\":\"120.0\""));
  // The arena unloads, the lobby is still loaded: 90 again. Then every level goes: the session rate.
  ClearGame();
  PutLevel(0, 0, CSymbol64Hash("mpl_lobby_b_combat"));
  Frame();
  QCHECK(g_asks.size() == 3U && g_asks[2].hz == 90.0f);
  ClearGame();
  Frame();
  QCHECK(g_asks.size() == 4U && g_asks[3].hz == 120.0f);
  sw = Lines("combat_refresh_rate_switch");
  QCHECK(sw.size() == 4U && Has(sw[3].text, "no combat or lobby level: session rate"));
  QCHECK(Has(sw[3].text, "\"level\":\"0000000000000000\""));  // no level: zero, written in full
  QCHECK(rr::CurrentStats().switches == 4);
  Done();
}

void TheTickLooksTwiceASecondAtMost() {
  World(bv::Variant::kCombat);
  InstallRule();
  GameAsks(120.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  Frame(0.2);  // the first look happens at once (the schedule starts empty) but a level change within 0.5 s of a look waits
  const std::size_t first = g_asks.size();
  ClearGame();
  Frame(0.2);
  Frame(0.2);
  QCHECK(g_asks.size() == first);  // 0.4 s after the look: not looked at yet
  Frame(0.2);
  QCHECK(g_asks.size() == first + 1);  // 0.6 s: the level left, the session rate comes back
  Done();
}

void NothingIsAskedWhileTheGamesRateIsAtOrBelowTheLevelRate() {
  World(bv::Variant::kCombat);
  InstallRule();
  GameAsks(90.0f);  // the game asks for 90 already (a headset that offers no more)
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  Frame();
  QCHECK(g_asks.empty());
  QCHECK(GameAsks(90.0f) == 90.0f);
  Done();

  World(bv::Variant::kCombat, Keys(true, true, true), /*quest2=*/1);  // a Quest 2: level rate 72, the game asks 90
  InstallRule();
  QCHECK(GameAsks(90.0f) == 90.0f);  // no level: as asked
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  Frame();
  QCHECK(g_asks.size() == 1U && g_asks[0].hz == 72.0f);
  QCHECK(GameAsks(90.0f) == 72.0f);
  Done();
}

void TheRuleOffNeverChangesARateAndLogsTheWitness() {
  World(bv::Variant::kCombat, Keys(false, true, true), 0);
  InstallRule();
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  QCHECK(GameAsks(120.0f) == 120.0f);
  Frame();
  QCHECK(g_asks.empty());
  QCHECK(Lines("combat_refresh_rate_request").size() == 1U);  // the witness: every request of the game is still logged
  QCHECK(Lines("combat_refresh_rate_switch").empty());
  Done();
}

void TheCombatOnlyModeDoesNotCountTheLobby() {
  World(bv::Variant::kCombat, Keys(true, false, true), 0);
  InstallRule();
  GameAsks(120.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_lobby_b_combat"));
  Frame();
  QCHECK(g_asks.empty());  // a lobby level asks for nothing with refresh_rate_lobby off
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_fission"));
  Frame();
  QCHECK(g_asks.size() == 1U && g_asks[0].hz == 90.0f);
  Done();
}

// An arena match on the combat APK: the APK is active, but the loaded level is an Echo Arena level, so the game's own rate stands whatever it
// asks, with the lobby key on or off, and nothing is re-asked. Combat then arena again: the rate comes back in both directions.
void AnArenaMatchOnTheCombatApkKeepsTheGamesRate() {
  for (const bool lobby : {true, false}) {
    World(bv::Variant::kCombat, Keys(true, lobby, true), 0);
    InstallRule();
    PutLevel(0, 0, CSymbol64Hash("mpl_arena_a"));
    QCHECK(GameAsks(120.0f) == 120.0f);
    Frame();
    Frame();
    QCHECK(g_asks.empty());
    QCHECK(GameAsks(120.0f) == 120.0f);  // asked again mid-match: still the game's
    QCHECK(!rr::CurrentStats().inLevel && rr::CurrentStats().switches == 0);
    // Arena to combat: 90; combat back to arena: the session rate, in both directions.
    PutLevel(0, 0, CSymbol64Hash("mpl_combat_gauss"));
    Frame();
    QCHECK(g_asks.size() == 1U && g_asks[0].hz == 90.0f);
    QCHECK(GameAsks(120.0f) == 90.0f);
    PutLevel(0, 0, CSymbol64Hash("mpl_arena_b"));
    Frame();
    QCHECK(g_asks.size() == 2U && g_asks[1].hz == 120.0f);  // a combat map unloaded for an arena level: the rate comes back
    QCHECK(GameAsks(120.0f) == 120.0f);
    QCHECK(!rr::CurrentStats().inLevel);
    Done();
  }
}

// The lobbies arena players use are not combat's: with the lobby key on or off, the game's own rate stays in each of them, and the combat lobby
// asks for the level rate only with the key on.
void TheLobbiesArenaPlayersUseKeepTheGamesRate() {
  for (const bool lobby : {true, false}) {
    for (const char* name : {"mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b2_lowspec", "mpl_lobby_b_arena", "mpl_lobby_b_social"}) {
      World(bv::Variant::kCombat, Keys(true, lobby, true), 0);
      InstallRule();
      PutLevel(0, 0, CSymbol64Hash(name));
      QCHECK(GameAsks(120.0f) == 120.0f);
      Frame();
      Frame();
      QCHECK(g_asks.empty());
      QCHECK(GameAsks(120.0f) == 120.0f);
      QCHECK(!rr::CurrentStats().inLevel && rr::CurrentStats().switches == 0);
      Done();
    }
    World(bv::Variant::kCombat, Keys(true, lobby, true), 0);
    InstallRule();
    PutLevel(0, 0, CSymbol64Hash("mpl_lobby_b_combat"));
    GameAsks(120.0f);
    Frame();
    QCHECK(g_asks.size() == (lobby ? 1U : 0U));  // the combat lobby: only with the key on
    QCHECK(GameAsks(120.0f) == (lobby ? 90.0f : 120.0f));
    Done();
  }
}

// The plugin's kill files are not read: a file found is a Warning that names the key replacing it, and the rule is what the keys say.
void ALegacyKillFileIsWarnedAboutAndChangesNothing() {
  char dir[] = "/tmp/refresh_rate_legacy_XXXXXX";
  QCHECK(mkdtemp(dir) != nullptr);
  const std::string base = dir;
  World(bv::Variant::kCombat, Keys(true, true, true), 0);
  rr::SetLegacyDirForTest(dir);
  InstallRule();
  QCHECK(Lines("combat_refresh_rate_legacy_switch").empty());  // no file, no line
  Done();
  struct Legacy {
    const char* file;
    const char* replacement;
  };
  for (const Legacy& legacy : {Legacy{"hzlevel.off", "combat.refresh_rate: false"}, Legacy{"hzlobby90.off", "combat.refresh_rate_lobby: false (the default)"},
                               Legacy{"hz72q2.off", "combat.refresh_rate_quest2_72: false"}}) {
    const std::string path = base + "/" + legacy.file;
    std::FILE* f = std::fopen(path.c_str(), "w");
    QCHECK(f != nullptr);
    if (f != nullptr) std::fclose(f);
    World(bv::Variant::kCombat, Keys(true, true, true), 0);
    rr::SetLegacyDirForTest(dir);
    const combat::ModuleResult r = InstallRule();
    QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);  // the file changed nothing: the rule is on
    const std::vector<Line> w = Lines("combat_refresh_rate_legacy_switch");
    QCHECK(w.size() == 1U);
    if (w.size() == 1U) {
      QCHECK(w[0].level == sentinel::LogLevel::kWarn);
      QCHECK(Has(w[0].text, std::string("\"file\":\"") + legacy.file + "\""));
      QCHECK(Has(w[0].text, std::string("\"replaced_by\":\"") + legacy.replacement + "\""));
      QCHECK(Has(w[0].text, "\"msg\":"));
    }
    QCHECK(Has(Lines("combat_refresh_rate_install")[0].text, "\"mode\":\"lobby\""));
    Done();
    std::remove(path.c_str());
  }
  rr::SetLegacyDirForTest(nullptr);
  rmdir(dir);
}

// The gate is the owner's first word, not only the release's absence of a published rule: with the rule published and a level loaded, a
// process where combat is not active answers the game's own rate and writes nothing.
void TheGateComesBeforeThePublishedRule() {
  World(bv::Variant::kCombat);
  InstallRule();
  GameAsks(120.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  Frame();
  QCHECK(g_asks.size() == 1U && rr::CurrentStats().inLevel);
  QCHECK(rr::Choose(kOvr, 120.0f) == 90.0f);  // active: kept at the level rate
  combat::ResetForTest();
  combat::SetVariant(bv::Variant::kNone);
  combat::Resolve(/*devBuild=*/false, /*forceAsked=*/false);
  QCHECK(!combat::Active());
  g_lines.clear();
  QCHECK(rr::Choose(kOvr, 120.0f) == 120.0f);  // not active: the game's own rate, whatever the rule holds
  rr::Note(120.0f, 120.0f, 0);
  QCHECK(Lines("combat_refresh_rate_request").empty());
  Done();
}

// The production re-ask: it reads the word now in vrapi_SetDisplayRefreshRate's slot of libr15 at every ask and calls it, so a hook installed on
// the slot after the module installed (the render-configuration thunk) sees the call. A missing image or an empty word asks nothing and
// answers -1.
struct SlotFixture {
  alignas(16) unsigned char mem[0x4000] = {};
  sentinel::ElfImage image{};
};
SlotFixture* g_slotFixture = nullptr;
std::vector<float> g_slotCalls;
int g_firstWordCalls = 0;
int FirstWord(void*, float hz) noexcept {
  ++g_firstWordCalls;
  g_slotCalls.push_back(hz);
  return 11;
}
int SecondWord(void*, float hz) noexcept {
  g_slotCalls.push_back(hz + 1000.0f);
  return 22;
}
bool FixtureLookup(const char* module, sentinel::ElfImage* out) {
  if (g_slotFixture == nullptr || std::strcmp(module, "libr15.so") != 0) return false;
  *out = g_slotFixture->image;
  return true;
}
// The pinned slot's offset from the image base: far beyond a small fixture, so the base is chosen to put the slot inside it.
void** SlotIn(SlotFixture& f) {
  const std::uint64_t slot = *sentinel::pinned::LibR15SetDisplayRefreshRate().slotVaddr;
  return reinterpret_cast<void**>(f.image.base + slot);
}

void TheProductionReaskCallsTheWordNowInTheSlot() {
  // A block big enough to hold the pinned slot: the slot's vaddr is 0x36ec5f0, so map that much (sparse, untouched pages are free).
  const std::size_t size = 0x3800000;
  void* const block = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  QCHECK(block != MAP_FAILED);
  static SlotFixture fixture;
  fixture.image.base = reinterpret_cast<std::uintptr_t>(block);
  g_slotFixture = &fixture;
  g_slotCalls.clear();
  g_firstWordCalls = 0;

  World(bv::Variant::kCombat);
  rr::SetAskForTest(nullptr);  // the production ask
  rr::SetImageLookupForTest(&FixtureLookup);
  InstallRule();
  GameAsks(120.0f);  // the session rate and the ovr pointer
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));

  *SlotIn(fixture) = reinterpret_cast<void*>(&FirstWord);
  Frame();
  QCHECK(g_slotCalls.size() == 1U && g_slotCalls[0] == 90.0f && g_firstWordCalls == 1);

  // A word put in the slot afterwards (a hook installed later) is the one the next re-ask reaches, not the word the slot held before.
  *SlotIn(fixture) = reinterpret_cast<void*>(&SecondWord);
  ClearGame();
  Frame();
  QCHECK(g_slotCalls.size() == 2U && g_slotCalls[1] == 1120.0f && g_firstWordCalls == 1);
  QCHECK(Lines("combat_refresh_rate_switch").size() == 2U && Has(Lines("combat_refresh_rate_switch")[1].text, "\"rc\":22"));

  // An empty word: nothing is called, and the switch line says the answer was -1.
  *SlotIn(fixture) = nullptr;
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  Frame();
  QCHECK(g_slotCalls.size() == 2U);
  const std::vector<Line> sw = Lines("combat_refresh_rate_switch");
  QCHECK(sw.size() == 3U && Has(sw[2].text, "\"rc\":-1"));

  // No libr15: the same.
  g_slotFixture = nullptr;
  ClearGame();
  Frame();
  QCHECK(g_slotCalls.size() == 2U && Lines("combat_refresh_rate_switch").size() == 4U);
  QCHECK(Has(Lines("combat_refresh_rate_switch")[3].text, "\"rc\":-1"));  // no image: the answer is -1 here too, not a call's result
  rr::SetImageLookupForTest(nullptr);
  Done();
  munmap(block, size);
}

// What the module reports about the last request and the last send, and the request line's level and result code.
void TheStatsAndTheRequestLineReportWhatWasAskedAndSent() {
  World(bv::Variant::kCombat, Keys(true, true, true), 0);
  InstallRule();
  g_lines.clear();
  rr::Note(100.0f, 80.0f, 5);  // a request the game made: 100 asked, 80 sent, the call answered 5
  rr::Stats stats = rr::CurrentStats();
  QCHECK(stats.requests == 1 && stats.lastAsked == 100.0f && stats.lastSent == 80.0f);
  const std::vector<Line> req = Lines("combat_refresh_rate_request");
  QCHECK(req.size() == 1U);
  if (req.size() == 1U) {
    QCHECK(req[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(req[0].text, "\"rc\":5") && Has(req[0].text, "\"asked_hz\":\"100.0\"") && Has(req[0].text, "\"sent_hz\":\"80.0\""));
  }
  QCHECK(GameAsks(120.0f) == 120.0f);
  stats = rr::CurrentStats();
  QCHECK(stats.requests == 2 && stats.lastAsked == 120.0f && stats.lastSent == 120.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_gauss"));
  Frame();
  stats = rr::CurrentStats();  // the tick's re-ask sets what was sent, not what the game asked
  QCHECK(stats.switches == 1 && stats.lastAsked == 120.0f && stats.lastSent == 90.0f && stats.inLevel);
  ClearGame();
  Frame();
  stats = rr::CurrentStats();
  QCHECK(stats.switches == 2 && stats.lastAsked == 120.0f && stats.lastSent == 120.0f && !stats.inLevel);
  Done();
}

// The headset reaches the request and the switch lines too, and the Quest 2's level rate is what the tick sends.
void ATickOnAQuest2SaysSoOnEveryLine() {
  World(bv::Variant::kCombat, Keys(true, true, true), /*quest2=*/1);
  InstallRule();
  GameAsks(90.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_gauss"));
  Frame();
  const std::vector<Line> req = Lines("combat_refresh_rate_request");
  const std::vector<Line> sw = Lines("combat_refresh_rate_switch");
  QCHECK(req.size() == 1U && Has(req[0].text, "\"quest2\":1"));
  QCHECK(sw.size() == 1U);
  if (sw.size() == 1U) {
    QCHECK(Has(sw[0].text, "\"quest2\":1") && Has(sw[0].text, "\"asked_hz\":\"90.0\"") && Has(sw[0].text, "\"sent_hz\":\"72.0\""));
  }
  Done();
}

// The install line's message says which of the three things the rule is doing.
void TheInstallMessageSaysWhatTheRuleDoes() {
  World(bv::Variant::kCombat, Keys(false, true, true), 0);
  InstallRule();
  std::vector<Line> l = Lines("combat_refresh_rate_install");
  QCHECK(l.size() == 1U && Has(l[0].text, "refresh_rate is off: the game's own rate always, every request logged"));
  Done();
  World(bv::Variant::kCombat, Keys(true, true, true), 0);
  InstallRule();
  l = Lines("combat_refresh_rate_install");
  QCHECK(l.size() == 1U && Has(l[0].text, "combat maps (and, in lobby mode, the combat lobby) ask for the level rate; an Echo Arena level keeps the game's rate"));
  Done();
  World(bv::Variant::kCombat);
  for (std::size_t i = 0; i < combat::tick::kMaxCallbacks; ++i) combat::tick::Add([](std::uintptr_t) noexcept {});
  InstallRule();
  l = Lines("combat_refresh_rate_install");
  QCHECK(l.size() == 1U && Has(l[0].text, "the game-thread tick table is full, so the rule only lowers requests the game makes; it never re-asks on a level change"));
  Done();
}

// The tick looks when 0.5 s have passed, not before: exactly 0.5 s after a look is a look, a hair before is not.
void TheTickLooksAtExactlyHalfASecond() {
  World(bv::Variant::kCombat);
  InstallRule();
  GameAsks(120.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  g_now = 101.0;
  rr::Tick(GameAddress());  // the look: schedules the next at 101.5
  QCHECK(g_asks.size() == 1U);
  ClearGame();
  g_now = 101.49;
  rr::Tick(GameAddress());
  QCHECK(g_asks.size() == 1U);  // 0.49 s: no look
  g_now = 101.5;
  rr::Tick(GameAddress());
  QCHECK(g_asks.size() == 2U && g_asks[1].hz == 120.0f);  // 0.5 s: the look
  Done();
}

// A request that came with no ovr pointer leaves the tick nothing to ask through.
void ATickWithoutTheGamesOvrAsksNothing() {
  World(bv::Variant::kCombat);
  InstallRule();
  QCHECK(rr::Choose(nullptr, 120.0f) == 120.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  Frame();
  QCHECK(g_asks.empty() && !rr::CurrentStats().inLevel);
  Done();
}

// With no lookup of the test's own, the re-ask finds libr15 through the loader's lookup: on a host there is none, and the answer is -1.
void TheProductionReaskWithoutALookupFindsNoImage() {
  World(bv::Variant::kCombat);
  rr::SetAskForTest(nullptr);  // the production ask
  InstallRule();
  GameAsks(120.0f);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  Frame();
  const std::vector<Line> sw = Lines("combat_refresh_rate_switch");
  QCHECK(sw.size() == 1U && Has(sw[0].text, "\"rc\":-1"));
  Done();
}

}  // namespace

int main() {
  TheRuleFollowsItsThreeKeysAndTheHeadset();
  TheHeadsetIsTheIdentityStepsProfile();
  ARequestAboveTheLevelRateInALevelIsKept();
  ThePlainReleaseHasNoOwnerAndTheGamesRateIsUntouched();
  ACombatVariantApkOwnsTheRuleAndSaysWhatItCombined();
  TheInstallLineFollowsTheKeysAndTheHeadset();
  AFullTickTableIsNamedAndTheModuleSaysItDidNotInstall();
  AComSessionFromArenaToCombatAndBack();
  TheTickLooksTwiceASecondAtMost();
  NothingIsAskedWhileTheGamesRateIsAtOrBelowTheLevelRate();
  TheRuleOffNeverChangesARateAndLogsTheWitness();
  TheCombatOnlyModeDoesNotCountTheLobby();
  TheProductionReaskCallsTheWordNowInTheSlot();
  TheGateComesBeforeThePublishedRule();
  AnArenaMatchOnTheCombatApkKeepsTheGamesRate();
  TheLobbiesArenaPlayersUseKeepTheGamesRate();
  ALegacyKillFileIsWarnedAboutAndChangesNothing();
  TheStatsAndTheRequestLineReportWhatWasAskedAndSent();
  ATickOnAQuest2SaysSoOnEveryLine();
  TheInstallMessageSaysWhatTheRuleDoes();
  TheTickLooksAtExactlyHalfASecond();
  ATickWithoutTheGamesOvrAsksNothing();
  TheProductionReaskWithoutALookupFindsNoImage();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "refresh_rate_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("refresh_rate_test: all checks passed");
  return 0;
}
