// Host test of pfxtev (src/quest/combat/pfxtev.cpp, #2188, #1642): the switch, the six event ids, the
// non-event pass-through, and one line per event-id decision. The assertions are the plugin test's; the fixtures are the combat
// area's (the key `combat.pfxtev` for the kill file, the sentinel's log sink). New with the move: the decision is scoped to combat
// maps (`AllowsIn`), because arena runs on the same APK, and the plugin's `pfxtev.off` file is no longer read.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/pfxtev.h"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/test_check.h"

namespace {

namespace tev = nevr_quest::combat::pfxtev;

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
int Count(const char* event, int level = -1) {
  int c = 0;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key) && (level < 0 || static_cast<int>(l.level) == level)) ++c;
  }
  return c;
}
// The text of the last line of an event ("" when there is none).
std::string LastText(const char* event) {
  const std::string key = std::string("\"event\":\"") + event + "\"";
  std::string out;
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out = l.text;
  }
  return out;
}
constexpr int kInfo = static_cast<int>(sentinel::LogLevel::kInfo);
constexpr int kWarn = static_cast<int>(sentinel::LogLevel::kWarn);

void Fresh() {
  g_lines.clear();
  tev::ResetForTest();
  tev::SetLegacyDirForTest(nullptr);
}

constexpr std::uint64_t kNonEvent = 0x66497bb6ull;  // not an event id: never gated by this switch

// Each id is the CSymbol64 of the name of its effect, so a changed bit in the table is caught (the count alone would not).
void TheEventTierHasSixIdsThatAreTheHashesOfTheirEffectNames() {
  QCHECK(tev::kIdCount == 6);
  const char* const names[] = {"pfx_test_combat_plrdeath_s10_torso_follow", "pfx_test_combat_plrdeath_s10_leg1_follow",
                               "pfx_test_combat_plrdeath_s10_leg2_follow",  "pfx_test_combat_plrdeath_s10_head_follow",
                               "pfx_test_combat_plrdeath_s10_arm2_follow",  "pfx_test_combat_plrdeath_s10_arm1_follow"};
  QCHECK(sizeof(names) / sizeof(names[0]) == tev::kIdCount);
  for (std::size_t i = 0; i < tev::kIdCount; ++i) QCHECK(tev::kIds[i] == EchoVR::CSymbol64Hash(names[i]));
}

// The first call of the process, before any Install or reset: the switch is on and the counters are zero (the module's statics, not a reset).
void TheSwitchIsOnInAFreshProcess() {
  QCHECK(tev::CurrentStats().allowed == 0 && tev::CurrentStats().refused == 0);
  QCHECK(tev::AllowsIn(tev::kIds[0], true));
  QCHECK(tev::CurrentStats().allowed == 1 && tev::CurrentStats().refused == 0);
}

// Default on before the install (the decision is taken, then the counters and lines are cleared).
void TheSwitchIsOnBeforeTheInstall() {
  Fresh();
  QCHECK(tev::AllowsIn(tev::kIds[0], true));
  Fresh();
}

void WithTheSwitchOnEveryEventIdIsAllowedAndEachDecisionIsOneLine() {
  Fresh();
  QCHECK(tev::Install(true).installed);
  QCHECK(Count("combat_pfxtev_install", kInfo) == 1);
  for (std::size_t i = 0; i < tev::kIdCount; ++i) QCHECK(tev::AllowsIn(tev::kIds[i], true));
  QCHECK(tev::AllowsIn(kNonEvent, true));  // a non-event id is not gated
  QCHECK(Count("combat_pfxtev_decision", kInfo) == 6);  // one INFO line per event-id decision, none for the non-event id
  QCHECK(tev::CurrentStats().allowed == 6 && tev::CurrentStats().refused == 0);
}

void WithTheSwitchOffEveryEventIdIsRefusedAndTheNonEventPasses() {
  Fresh();
  const nevr_quest::combat::ModuleResult r = tev::Install(false);
  QCHECK(!r.installed && std::string(r.status) == "off_config");
  QCHECK(Count("combat_pfxtev_install", kInfo) == 1);  // a choice, not a failure
  for (std::size_t i = 0; i < tev::kIdCount; ++i) QCHECK(!tev::AllowsIn(tev::kIds[i], true));
  QCHECK(tev::AllowsIn(kNonEvent, true));
  QCHECK(tev::CurrentStats().refused == 6 && tev::CurrentStats().allowed == 0);
}

void PastTheCapOneWarningIsWrittenAndTheRestAreOnlyCounted() {
  Fresh();
  tev::Install(true);
  for (int i = 0; i < 64; ++i) tev::AllowsIn(tev::kIds[0], true);
  QCHECK(Count("combat_pfxtev_decision", kInfo) == 64);
  QCHECK(Count("combat_pfxtev_log_cap") == 0);  // the 64th decision is still a line; the Warning is the 65th's
  tev::AllowsIn(tev::kIds[0], true);
  QCHECK(Count("combat_pfxtev_log_cap", kWarn) == 1);
  for (int i = 65; i < 100; ++i) tev::AllowsIn(tev::kIds[0], true);
  QCHECK(Count("combat_pfxtev_decision", kInfo) == 64);
  QCHECK(Count("combat_pfxtev_log_cap") == 1);
  QCHECK(tev::CurrentStats().allowed == 100);
  // The Warning comes after the 64th line and says what was capped.
  int decisionsBefore = 0;
  bool seen = false;
  for (const Line& l : g_lines) {
    if (Has(l.text, "\"event\":\"combat_pfxtev_log_cap\"")) {
      seen = true;
      QCHECK(Has(l.text, "\"cap\":64") && Has(l.text, "counted but not written"));
      break;
    }
    if (Has(l.text, "\"event\":\"combat_pfxtev_decision\"")) ++decisionsBefore;
  }
  QCHECK(seen && decisionsBefore == 64);
}

// Past the cap a decision is still made and counted, and a refused one is still refused: the cap limits the lines, not the answers.
void PastTheCapTheAnswersAreStillRight() {
  Fresh();
  tev::Install(false);  // the switch off: every event id refused
  for (int i = 0; i < 100; ++i) QCHECK(!tev::AllowsIn(tev::kIds[2], true));
  QCHECK(tev::CurrentStats().refused == 100 && tev::CurrentStats().allowed == 0);
  Fresh();
  tev::Install(true);  // the switch on, outside a combat map: refused
  for (int i = 0; i < 100; ++i) QCHECK(!tev::AllowsIn(tev::kIds[2], false));
  QCHECK(tev::CurrentStats().refused == 100 && tev::CurrentStats().allowed == 0);
  Fresh();
  tev::Install(true);  // allowed past the cap too
  for (int i = 0; i < 100; ++i) QCHECK(tev::AllowsIn(tev::kIds[2], true));
  QCHECK(tev::CurrentStats().allowed == 100);
}

// The install line states the switch, why, which key and what it means.
void TheInstallLineStatesTheSwitchAndItsConsequence() {
  Fresh();
  const nevr_quest::combat::ModuleResult on = tev::Install(true);
  QCHECK(on.installed && std::string(on.status) == "installed");
  const std::string onText = LastText("combat_pfxtev_install");
  QCHECK(Has(onText, "\"state\":\"on\"") && Has(onText, "\"reason\":\"default\"") && Has(onText, "\"switch\":\"combat.pfxtev\""));
  QCHECK(Has(onText, "may be created in combat maps") && !Has(onText, "are refused"));
  Fresh();
  const nevr_quest::combat::ModuleResult off = tev::Install(false);
  QCHECK(!off.installed && std::string(off.status) == "off_config");
  const std::string offText = LastText("combat_pfxtev_install");
  QCHECK(Has(offText, "\"state\":\"off\"") && Has(offText, "\"reason\":\"off_config\"") && Has(offText, "\"switch\":\"combat.pfxtev\""));
  QCHECK(Has(offText, "are refused") && !Has(offText, "may be created"));
}

// A decision line names the id (16 lower-case hex digits), the answer, and why: the switch when it is off (whatever the level), else the level.
void TheDecisionLineNamesTheIdTheAnswerAndTheReason() {
  struct Case {
    bool switchOn, inCombatMap;
    const char* result;
    const char* reason;
  };
  const Case cases[] = {{true, true, "allowed", "switch"}, {true, false, "refused", "level"}, {false, true, "refused", "switch"}, {false, false, "refused", "switch"}};
  for (const Case& c : cases) {
    Fresh();
    tev::Install(c.switchOn);
    g_lines.clear();
    QCHECK(tev::AllowsIn(tev::kIds[1], c.inCombatMap) == (std::string(c.result) == "allowed"));
    QCHECK(Count("combat_pfxtev_decision", kInfo) == 1);
    const std::string text = LastText("combat_pfxtev_decision");
    char id[24];
    std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(tev::kIds[1]));
    QCHECK(Has(text, std::string("\"id\":\"") + id + "\""));
    QCHECK(Has(text, std::string("\"result\":\"") + c.result + "\""));
    QCHECK(Has(text, std::string("\"reason\":\"") + c.reason + "\""));
    if (std::string(c.result) == "allowed") {
      QCHECK(Has(text, "may create this event effect") && !Has(text, "may not create"));
    } else {
      QCHECK(Has(text, "may not create this event effect"));
    }
  }
}

// Arena shares the APK: an event id is created only while a combat map is loaded; outside one it is refused even with the switch on, and
// a non-event id is not this switch's whatever the level.
void AnEventIdIsAllowedOnACombatMapOnly() {
  Fresh();
  tev::Install(true);
  for (std::size_t i = 0; i < tev::kIdCount; ++i) {
    QCHECK(tev::AllowsIn(tev::kIds[i], true));
    QCHECK(!tev::AllowsIn(tev::kIds[i], false));
  }
  QCHECK(tev::AllowsIn(kNonEvent, true) && tev::AllowsIn(kNonEvent, false));
  QCHECK(tev::CurrentStats().allowed == 6 && tev::CurrentStats().refused == 6);
  QCHECK(Count("combat_pfxtev_decision") == 12);
  bool level = false;
  for (const Line& l : g_lines) {
    if (Has(l.text, "\"reason\":\"level\"")) level = true;
  }
  QCHECK(level);  // the refusal names why: the level, not the switch
}

void TheKillFileIsNotReadAndIsWarnedAbout() {
  Fresh();
  char dir[] = "/tmp/pfxtev_legacy_XXXXXX";
  QCHECK(mkdtemp(dir) != nullptr);
  tev::SetLegacyDirForTest(dir);
  tev::Install(true);
  QCHECK(Count("combat_pfxtev_legacy_switch") == 0);
  const std::string file = std::string(dir) + "/pfxtev.off";
  if (std::FILE* f = std::fopen(file.c_str(), "w")) std::fclose(f);
  Fresh();
  tev::SetLegacyDirForTest(dir);
  tev::Install(true);
  QCHECK(tev::AllowsIn(tev::kIds[0], true));  // the file turned nothing off
  QCHECK(Count("combat_pfxtev_legacy_switch", kWarn) == 1);
  bool named = false;
  for (const Line& l : g_lines) {
    if (Has(l.text, "combat_pfxtev_legacy_switch") && Has(l.text, "combat.pfxtev: false")) named = true;
  }
  QCHECK(named);
  const std::string legacy = LastText("combat_pfxtev_legacy_switch");
  QCHECK(Has(legacy, "\"file\":\"pfxtev.off\"") && Has(legacy, "no longer read and changes nothing"));
  std::remove(file.c_str());
  rmdir(dir);
  tev::SetLegacyDirForTest(nullptr);
}

// The cost of a decision (ADR 0038), printed not asserted: a non-event id (no line, the case every other effect takes) and a refused event id
// outside a combat map past the log cap (the counting path an arena match would take).
void PrintTheCostOfADecision() {
  Fresh();
  tev::Install(true);
  for (int i = 0; i < 100; ++i) tev::AllowsIn(tev::kIds[0], false);  // past the cap: no more lines
  constexpr int kCalls = 200000;
  volatile bool sink = false;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kCalls; ++i) sink = tev::AllowsIn(kNonEvent, false);
  auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < kCalls; ++i) sink = tev::AllowsIn(tev::kIds[3], false);
  auto t2 = std::chrono::steady_clock::now();
  (void)sink;
  const auto ns = [&](auto a, auto b) { return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()) / kCalls; };
  std::printf("pfxtev_test: cost on the host: a non-event id %.1f ns/call, an event id refused past the log cap %.1f ns/call, %d calls each\n",
              ns(t0, t1), ns(t1, t2), kCalls);
}

}  // namespace

int main() {
  const sentinel::LogSink previous = sentinel::SetLogSink(&Capture);
  TheSwitchIsOnInAFreshProcess();  // first: nothing has reset the module's state yet
  TheEventTierHasSixIdsThatAreTheHashesOfTheirEffectNames();
  TheSwitchIsOnBeforeTheInstall();
  WithTheSwitchOnEveryEventIdIsAllowedAndEachDecisionIsOneLine();
  WithTheSwitchOffEveryEventIdIsRefusedAndTheNonEventPasses();
  PastTheCapOneWarningIsWrittenAndTheRestAreOnlyCounted();
  PastTheCapTheAnswersAreStillRight();
  TheInstallLineStatesTheSwitchAndItsConsequence();
  TheDecisionLineNamesTheIdTheAnswerAndTheReason();
  AnEventIdIsAllowedOnACombatMapOnly();
  TheKillFileIsNotReadAndIsWarnedAbout();
  PrintTheCostOfADecision();
  sentinel::SetLogSink(previous);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "pfxtev_test: %d failure(s)\n", quest_test::Failures());
    return 1;
  }
  std::printf("pfxtev_test: OK\n");
  return 0;
}
