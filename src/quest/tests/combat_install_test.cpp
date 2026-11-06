// Host test for the combat area's two install steps (#2171, docs/adr/0046 rules 5 and 6): InstallEarly and InstallNormal each run one
// registration table in its own order, only when `combat::Active()` says combat runs in this process, and each writes one
// `combat_install` aggregate line (phase, variant, active, each module and its result). The table is injected here (RunPhase), so the
// rules are tested before any module has moved in: the order, the skip of a module with no step in the phase, one line per phase, a
// failure counted and named, a plain release that installs nothing and says so once, and the `not_started` record of a normal step
// that never got its moment. LogConfig writes the `combat_config` line.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/combat_install.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/combat/combat_active.h"
#include "quest/tests/test_check.h"

namespace {

namespace bv = nevr_quest::build_variant;
namespace combat = nevr_quest::combat;

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// The lines of one event, in order.
std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}

std::vector<std::string> g_calls;
combat::ModuleResult Ok() noexcept { return {true, "installed"}; }
combat::ModuleResult Refused() noexcept { return {false, "build_id_mismatch"}; }
combat::ModuleResult EarlyA() noexcept { g_calls.emplace_back("a_early"); return Ok(); }
combat::ModuleResult NormalA() noexcept { g_calls.emplace_back("a_normal"); return Ok(); }
combat::ModuleResult NormalImpact() noexcept { g_calls.emplace_back("impactsnd"); return Ok(); }
combat::ModuleResult NormalDecals() noexcept { g_calls.emplace_back("decals"); return Ok(); }
combat::ModuleResult NormalFails() noexcept { g_calls.emplace_back("fails"); return Refused(); }

// Combat runs in this process (a combat APK) or does not (a plain release).
void Process(bv::Variant variant) {
  combat::ResetForTest();
  combat::ResetInstallForTest();
  g_lines.clear();
  g_calls.clear();
  combat::SetVariant(variant);
  sentinel::SetLogSink(&Capture);
  combat::Resolve(/*devBuild=*/false, /*forceAsked=*/false);
  g_lines.clear();  // the combat_active line is combat_active_test's
}

void Done() { sentinel::SetLogSink(nullptr); }

const combat::Module kTable[] = {
    {"alpha", &EarlyA, &NormalA},
    {"early_only", &EarlyA, nullptr},
    {"impactsnd", nullptr, &NormalImpact},
    {"decals", nullptr, &NormalDecals},  // after impactsnd: its shutdown wrapper chains on impactsnd's
};

void NamesAreStableTokens() {
  QCHECK(std::strcmp(combat::PhaseName(combat::Phase::kEarly), "early") == 0);
  QCHECK(std::strcmp(combat::PhaseName(combat::Phase::kNormal), "normal") == 0);
}

// The normal phase runs the modules that have a normal step, in table order (decals after impactsnd), and the early phase only the ones
// with an early step; a module with no step in a phase is not listed.
void EachPhaseRunsItsModulesInTableOrder() {
  Process(bv::Variant::kCombat);
  const combat::Outcome early = combat::RunPhase(combat::Phase::kEarly, kTable, 4);
  QCHECK(early.ran && early.modules == 2 && early.installed == 2 && early.failed == 0);
  QCHECK((g_calls == std::vector<std::string>{"a_early", "a_early"}));
  g_calls.clear();
  const combat::Outcome normal = combat::RunPhase(combat::Phase::kNormal, kTable, 4);
  QCHECK(normal.ran && normal.modules == 3 && normal.installed == 3 && normal.failed == 0);
  QCHECK((g_calls == std::vector<std::string>{"a_normal", "impactsnd", "decals"}));
  Done();
}

// One aggregate line per phase: the phase, the variant, that combat is active and why, every module with its result, the counts and the
// consequence (N17).
void EachPhaseWritesOneAggregateLine() {
  Process(bv::Variant::kQ15t);
  combat::RunPhase(combat::Phase::kNormal, kTable, 4);
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    const Line& l = lines[0];
    QCHECK(l.level == sentinel::LogLevel::kInfo);
    QCHECK(Has(l.text, "\"phase\":\"normal\""));
    QCHECK(Has(l.text, "\"state\":\"ran\""));
    QCHECK(Has(l.text, "\"variant\":\"q15t\""));
    QCHECK(Has(l.text, "\"active\":1"));
    QCHECK(Has(l.text, "\"active_reason\":\"variant\""));
    QCHECK(Has(l.text, "\"modules\":3"));
    QCHECK(Has(l.text, "\"installed\":3"));
    QCHECK(Has(l.text, "\"failed\":0"));
    QCHECK(Has(l.text, "\"results\":\"alpha=installed impactsnd=installed decals=installed\""));
    QCHECK(Has(l.text, "\"msg\":\"3 of 3 combat modules installed in this phase\""));
  }
  Done();
}

// A module that does not install is counted, named with its status, and makes the line a Warning; the others still install.
void AFailingModuleIsNamedAndTheOthersStillInstall() {
  Process(bv::Variant::kCombat);
  const combat::Module table[] = {{"first", nullptr, &NormalA}, {"broken", nullptr, &NormalFails}, {"last", nullptr, &NormalDecals}};
  const combat::Outcome o = combat::RunPhase(combat::Phase::kNormal, table, 3);
  QCHECK(o.ran && o.modules == 3 && o.installed == 2 && o.failed == 1);
  QCHECK((g_calls == std::vector<std::string>{"a_normal", "fails", "decals"}));
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(lines[0].text, "broken=build_id_mismatch"));
    QCHECK(Has(lines[0].text, "\"failed\":1"));
    QCHECK(Has(lines[0].text, "2 of 3 combat modules installed in this phase"));
  }
  Done();
}

// A plain release installs nothing of combat's and says so once: no module function runs, one line states that combat does not install in
// this run, and neither does a repeat of the call, nor the other phase, say it again.
void APlainReleaseInstallsNothingAndSaysSoOnce() {
  Process(bv::Variant::kNone);
  const combat::Outcome a = combat::RunPhase(combat::Phase::kEarly, kTable, 4);
  const combat::Outcome b = combat::RunPhase(combat::Phase::kEarly, kTable, 4);
  const combat::Outcome c = combat::RunPhase(combat::Phase::kNormal, kTable, 4);
  QCHECK(!a.ran && !b.ran && !c.ran);
  QCHECK(g_calls.empty());
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(lines[0].text, "\"state\":\"skipped\""));
    QCHECK(Has(lines[0].text, "\"variant\":\"none\""));
    QCHECK(Has(lines[0].text, "\"active\":0"));
    QCHECK(Has(lines[0].text, "\"active_reason\":\"not_a_combat_variant\""));
    QCHECK(Has(lines[0].text, "\"modules\":0"));
    QCHECK(Has(lines[0].text, "folded combat modules do not install in this run; the game keeps its own behaviour"));
  }
  Done();
}

// Before the configuration is resolved nothing is active, so nothing installs either.
void NothingInstallsBeforeTheAnswerIsMade() {
  combat::ResetForTest();
  combat::ResetInstallForTest();
  g_calls.clear();
  g_lines.clear();
  sentinel::SetLogSink(&Capture);
  combat::SetVariant(bv::Variant::kCombat);  // the variant alone is not the answer
  const combat::Outcome o = combat::RunPhase(combat::Phase::kEarly, kTable, 4);
  QCHECK(!o.ran && g_calls.empty());
  QCHECK(Has(Lines("combat_install").at(0).text, "\"active_reason\":\"unresolved\""));
  Done();
}

// A phase runs once per process: a second call neither runs a module nor writes a line.
void APhaseRunsOnce() {
  Process(bv::Variant::kCombat);
  combat::RunPhase(combat::Phase::kNormal, kTable, 4);
  g_calls.clear();
  const combat::Outcome again = combat::RunPhase(combat::Phase::kNormal, kTable, 4);
  QCHECK(!again.ran && g_calls.empty());
  QCHECK(Lines("combat_install").size() == 1U);
  Done();
}

// A table whose results do not fit the line's list keeps the counts exact and says the list was cut, rather than writing a broken line.
void ALongListIsCutAndSaysSoWhileTheCountsStayExact() {
  Process(bv::Variant::kCombat);
  static const combat::Module many[] = {
      {"module_with_a_long_name_01", nullptr, &NormalA}, {"module_with_a_long_name_02", nullptr, &NormalA},
      {"module_with_a_long_name_03", nullptr, &NormalA}, {"module_with_a_long_name_04", nullptr, &NormalA},
      {"module_with_a_long_name_05", nullptr, &NormalA}, {"module_with_a_long_name_06", nullptr, &NormalA},
      {"module_with_a_long_name_07", nullptr, &NormalA}, {"module_with_a_long_name_08", nullptr, &NormalA},
      {"module_with_a_long_name_09", nullptr, &NormalA}, {"module_with_a_long_name_10", nullptr, &NormalA},
      {"module_with_a_long_name_11", nullptr, &NormalA}, {"module_with_a_long_name_12", nullptr, &NormalA},
      {"module_with_a_long_name_13", nullptr, &NormalA}, {"module_with_a_long_name_14", nullptr, &NormalA},
  };
  const combat::Outcome o = combat::RunPhase(combat::Phase::kNormal, many, 14);
  QCHECK(o.modules == 14 && o.installed == 14 && g_calls.size() == 14U);  // every module still ran
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(Has(lines[0].text, "\"modules\":14") && Has(lines[0].text, "\"installed\":14"));
    QCHECK(Has(lines[0].text, "\"results_truncated\":1"));
    QCHECK(Has(lines[0].text, "module_with_a_long_name_01=installed"));
  }
  Done();
}

// The shipped table: sendguard has a step in the normal phase and none in the early one (#2179). On the host there is no libr15 to hook,
// so its row reports the refusal, and the line says so at Warning.
void TheShippedTableHoldsSendguardInTheNormalPhase() {
  Process(bv::Variant::kCombat);
  const combat::Outcome early = combat::InstallEarly();
  const combat::Outcome normal = combat::InstallNormal();
  QCHECK(early.ran && early.modules == 0 && normal.ran && normal.modules == 1);
  QCHECK(normal.installed == 0 && normal.failed == 1);
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 2U);
  if (lines.size() == 2U) {
    QCHECK(Has(lines[0].text, "\"phase\":\"early\"") && Has(lines[1].text, "\"phase\":\"normal\""));
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(lines[0].text, "\"modules\":0") && Has(lines[0].text, "\"results\":\"\""));
    QCHECK(Has(lines[0].text, "no combat module has a step in this phase yet; nothing of combat's is installed"));
    QCHECK(lines[1].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(lines[1].text, "\"modules\":1") && Has(lines[1].text, "\"results\":\"sendguard=module_not_loaded\""));
  }
  Done();
}

// The real steps in a plain release: nothing installs, and the one line is the skip.
void TheRealStepsConsultTheGate() {
  Process(bv::Variant::kNone);
  QCHECK(!combat::InstallEarly().ran);
  QCHECK(!combat::InstallNormal().ran);
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 1U);
  Done();
}

// A normal step that never got its moment is recorded once, with the reason; one that ran, or a process where combat is off, records nothing.
void ANormalStepThatNeverRanIsRecordedAsNotStartedOnce() {
  Process(bv::Variant::kCombat);
  combat::NoteNormalNotStarted(combat::reason::kPostLoadHookUnavailable);
  combat::NoteNormalNotStarted(combat::reason::kGameExitedFirst);  // the process ending after the sequence said so: once
  std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(lines[0].text, "\"phase\":\"normal\""));
    QCHECK(Has(lines[0].text, "\"state\":\"not_started\""));
    QCHECK(Has(lines[0].text, "\"reason\":\"post_load_hook_unavailable\""));
    QCHECK(Has(lines[0].text, "\"variant\":\"combat\"") && Has(lines[0].text, "\"active\":1"));
    QCHECK(Has(lines[0].text, "the game keeps its own behaviour"));
  }
  // The normal step that already ran is not "not started" when the process ends.
  Process(bv::Variant::kCombat);
  combat::InstallNormal();
  g_lines.clear();
  combat::NoteNormalNotStarted(combat::reason::kGameExitedFirst);
  QCHECK(Lines("combat_install").empty());
  // A plain release owes nothing, and the end of its process writes nothing.
  Process(bv::Variant::kNone);
  combat::NoteNormalNotStarted(combat::reason::kGameExitedFirst);
  QCHECK(g_lines.empty());
  // A later normal step after the record does not run: the record ended it.
  Process(bv::Variant::kCombat);
  combat::NoteNormalNotStarted(combat::reason::kPostLoadHookUnavailable);
  QCHECK(!combat::InstallNormal().ran);
  Done();
}

// The `combat_config` line: each key with its value and where it came from, once, at Info.
void TheConfigLineNamesEachKeyItsValueAndItsSource() {
  g_lines.clear();
  sentinel::SetLogSink(&Capture);
  nevr_quest::CombatConfig cfg;
  combat::LogConfig(cfg);
  cfg.force = {true, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  cfg.force = {false, nevr_quest::Source::kFile};  // the file said false: not asked, but the source is the file's
  cfg.sendguard = {false, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  const std::vector<Line> lines = Lines("combat_config");
  QCHECK(lines.size() == 3U);
  if (lines.size() == 3U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(lines[0].text, "\"force\":0") && Has(lines[0].text, "\"force_source\":\"default\""));
    // sendguard: on by default, from the default; a file's word, either way, is the file's
    QCHECK(Has(lines[0].text, "\"sendguard\":1") && Has(lines[0].text, "\"sendguard_source\":\"default\""));
    QCHECK(Has(lines[1].text, "\"force\":1") && Has(lines[1].text, "\"force_source\":\"config\""));
    QCHECK(Has(lines[2].text, "\"force\":0") && Has(lines[2].text, "\"force_source\":\"config\""));
    QCHECK(Has(lines[2].text, "\"sendguard\":0") && Has(lines[2].text, "\"sendguard_source\":\"config\""));
    QCHECK(combat::Config().sendguard.on == false && combat::Config().force.on == false);  // LogConfig kept it for the modules
    QCHECK(Has(lines[1].text, "\"msg\":"));
  }
  combat::ResetInstallForTest();
  QCHECK(combat::Config().sendguard.on && !combat::Config().force.on);  // forgotten: the shipped defaults, every switch on
  Done();
}

}  // namespace

int main() {
  NamesAreStableTokens();
  EachPhaseRunsItsModulesInTableOrder();
  EachPhaseWritesOneAggregateLine();
  AFailingModuleIsNamedAndTheOthersStillInstall();
  APlainReleaseInstallsNothingAndSaysSoOnce();
  NothingInstallsBeforeTheAnswerIsMade();
  APhaseRunsOnce();
  ALongListIsCutAndSaysSoWhileTheCountsStayExact();
  TheShippedTableHoldsSendguardInTheNormalPhase();
  TheRealStepsConsultTheGate();
  ANormalStepThatNeverRanIsRecordedAsNotStartedOnce();
  TheConfigLineNamesEachKeyItsValueAndItsSource();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("combat_install_test: all checks passed");
  return 0;
}
