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
#include "quest/combat/heaps.h"
#include "quest/combat/memsys.h"
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

// The real steps, with the table as it ships: heaps and memsys have an early step, in that order, and nothing has a normal one yet. No
// libr15 is loaded in this process, so each module refuses with its fixed token and the line names both; the normal phase writes its one
// line with no modules.
void TheShippedTableHoldsHeapsThenMemsysEarlyAndTheTickThenTheRefreshRateNormal() {
  nevr_quest::combat::heaps::ResetForTest();
  nevr_quest::combat::memsys::ResetForTest();
  Process(bv::Variant::kCombat);
  const combat::Outcome early = combat::InstallEarly();
  const combat::Outcome normal = combat::InstallNormal();
  QCHECK(early.ran && early.modules == 2 && early.installed == 0 && early.failed == 2);
  // The normal step runs the tick (combat_tick.h) first, then the combat level answer's refresh (#2186), the refresh-rate rule (#2172), the wrist
  // HUD and the shield colour (#2186), then the pfx gate and the event-effect decision (#2188). On the host no libr15 is loaded, so the tick is
  // refused and named; the refresh cannot register without it and the two hooks, which read the answer, refuse in turn; the rule installs (it
  // registers its callback for a tick that may come later), and the pfx gate and the event-effect decision, which hook nothing, install after it.
  QCHECK(normal.ran && normal.modules == 7 && normal.installed == 3 && normal.failed == 4);
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 2U);
  if (lines.size() == 2U) {
    QCHECK(Has(lines[0].text, "\"phase\":\"early\"") && Has(lines[1].text, "\"phase\":\"normal\""));
    QCHECK(lines[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(lines[0].text, "\"results\":\"heaps=module_not_loaded memsys=module_not_loaded\""));
    QCHECK(lines[1].level == sentinel::LogLevel::kWarn);  // the refused tick makes the line a Warning
    QCHECK(Has(lines[1].text, "\"results\":\"tick=module_not_loaded scope=tick_not_installed refresh_rate=installed hudmeter=scope_not_live shieldgamma=scope_not_live pfx=installed pfxtev=installed\""));
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
  cfg.refreshRateLobby = {false, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  cfg.force = {false, nevr_quest::Source::kFile};  // the file said false: not asked, but the source is the file's
  combat::LogConfig(cfg);
  const std::vector<Line> lines = Lines("combat_config");
  QCHECK(lines.size() == 3U);
  if (lines.size() == 3U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(lines[0].text, "\"force\":0") && Has(lines[0].text, "\"force_source\":\"default\""));
    // The refresh-rate rule's three keys: on by default (the lobby's is off), from the default; the file's word where it gave one.
    QCHECK(Has(lines[0].text, "\"refresh_rate\":1") && Has(lines[0].text, "\"refresh_rate_source\":\"default\""));
    QCHECK(Has(lines[0].text, "\"refresh_rate_lobby\":0") && Has(lines[0].text, "\"refresh_rate_lobby_source\":\"default\""));
    QCHECK(Has(lines[0].text, "\"refresh_rate_quest2_72\":1") && Has(lines[0].text, "\"refresh_rate_quest2_72_source\":\"default\""));
    QCHECK(Has(lines[1].text, "\"force\":1") && Has(lines[1].text, "\"force_source\":\"config\""));
    QCHECK(Has(lines[1].text, "\"refresh_rate_lobby\":0") && Has(lines[1].text, "\"refresh_rate_lobby_source\":\"config\""));
    QCHECK(Has(lines[1].text, "\"refresh_rate\":1"));
    // The particle-renderer gate and the event-effect decision (#2188): on by default, from the default.
    QCHECK(Has(lines[0].text, "\"pfx\":1") && Has(lines[0].text, "\"pfx_source\":\"default\""));
    QCHECK(Has(lines[0].text, "\"pfxtev\":1") && Has(lines[0].text, "\"pfxtev_source\":\"default\""));
    QCHECK(Has(lines[2].text, "\"force\":0") && Has(lines[2].text, "\"force_source\":\"config\""));
    QCHECK(Has(lines[1].text, "\"msg\":"));
    // The module switches: on by default, each with its source.
    QCHECK(Has(lines[0].text, "\"heaps\":1") && Has(lines[0].text, "\"heaps_source\":\"default\""));
    QCHECK(Has(lines[0].text, "\"memsys\":1") && Has(lines[0].text, "\"memsys_source\":\"default\""));
  }
  // The wrist HUD and the shield colour (#2186) are the second record: the HUD and the tube mirror on, the gamma off, each from the default.
  const std::vector<Line> modules = Lines("combat_config_modules");
  QCHECK(modules.size() == 3U);
  if (modules.size() == 3U) {
    QCHECK(modules[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(modules[0].text, "\"hudmeter\":1") && Has(modules[0].text, "\"hudmeter_source\":\"default\""));
    QCHECK(Has(modules[0].text, "\"shieldgamma_gamma\":0") && Has(modules[0].text, "\"shieldgamma_gamma_source\":\"default\""));
    QCHECK(Has(modules[0].text, "\"shieldgamma_tube_mirror\":1") && Has(modules[0].text, "\"shieldgamma_tube_mirror_source\":\"default\""));
    QCHECK(Has(modules[0].text, "\"msg\":") && !Has(modules[0].text, "truncated"));
  }
  g_lines.clear();
  cfg.heaps = {false, nevr_quest::Source::kFile};
  cfg.memsys = {true, nevr_quest::Source::kFile};
  cfg.hudmeter = {false, nevr_quest::Source::kFile};
  cfg.shieldgammaGamma = {true, nevr_quest::Source::kFile};
  cfg.shieldgammaTubeMirror = {false, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  const std::vector<Line> switches = Lines("combat_config");
  QCHECK(switches.size() == 1U);
  if (switches.size() == 1U) {
    QCHECK(Has(switches[0].text, "\"heaps\":0") && Has(switches[0].text, "\"heaps_source\":\"config\""));
    QCHECK(Has(switches[0].text, "\"memsys\":1") && Has(switches[0].text, "\"memsys_source\":\"config\""));
    QCHECK(!Has(switches[0].text, "truncated") && Has(switches[0].text, "\"msg\":"));  // the 768-byte line still holds its message
  }
  const std::vector<Line> moduleSwitches = Lines("combat_config_modules");
  QCHECK(moduleSwitches.size() == 1U);
  if (moduleSwitches.size() == 1U) {
    QCHECK(Has(moduleSwitches[0].text, "\"hudmeter\":0") && Has(moduleSwitches[0].text, "\"hudmeter_source\":\"config\""));
    QCHECK(Has(moduleSwitches[0].text, "\"shieldgamma_gamma\":1") && Has(moduleSwitches[0].text, "\"shieldgamma_gamma_source\":\"config\""));
    QCHECK(Has(moduleSwitches[0].text, "\"shieldgamma_tube_mirror\":0") && Has(moduleSwitches[0].text, "\"shieldgamma_tube_mirror_source\":\"config\""));
  }
  Done();
}

// The `combat:` keys the modules read at their install: the shipped defaults until the configuration step sets them, then what it set.
void TheRefreshRateRowReadsItsThreeKeysFromTheResolvedSection() {
  // Defaults: the rule on (installed, registers its tick), the combat lobby off.
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig defaults;
  QCHECK(defaults.refreshRate.on && !defaults.refreshRateLobby.on && defaults.refreshRateQuest2At72.on);
  combat::SetConfig(defaults);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_install")[0].text, "refresh_rate=installed"));
  QCHECK(Has(Lines("combat_refresh_rate_install")[0].text, "\"mode\":\"match\""));
  Done();
  // The file's word reaches the row: the rule off is the witness.
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig off;
  off.refreshRate = {false, nevr_quest::Source::kFile};
  combat::SetConfig(off);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_install")[0].text, "refresh_rate=witness"));
  Done();
  // And the lobby key on: lobby mode.
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig lobby;
  lobby.refreshRateLobby = {true, nevr_quest::Source::kFile};
  combat::SetConfig(lobby);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_refresh_rate_install")[0].text, "\"mode\":\"lobby\""));
  Done();
}

// The wrist HUD and shield colour rows read their keys from the resolved section (#2186): with the defaults each attempts its install (and, with
// no libr15 on the host, names the refusal); the file's word reaches the row, and the shield colour with both its switches false is off.
void TheHudmeterAndShieldgammaRowsReadTheirKeysFromTheResolvedSection() {
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig defaults;
  QCHECK(defaults.hudmeter.on && !defaults.shieldgammaGamma.on && defaults.shieldgammaTubeMirror.on);
  combat::SetConfig(defaults);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_install")[0].text, "hudmeter=scope_not_live shieldgamma=scope_not_live"));
  QCHECK(Lines("combat_hudmeter_install").size() == 1U && Has(Lines("combat_hudmeter_install")[0].text, "\"result\":\"refused\""));
  QCHECK(Has(Lines("combat_shieldgamma_install")[0].text, "\"gamma\":0,\"tube_mirror\":1"));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig off;
  off.hudmeter = {false, nevr_quest::Source::kFile};
  off.shieldgammaTubeMirror = {false, nevr_quest::Source::kFile};  // the gamma is off too: both false
  combat::SetConfig(off);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_install")[0].text, "hudmeter=off shieldgamma=off"));
  QCHECK(Has(Lines("combat_hudmeter_install")[0].text, "\"reason\":\"off_config\""));
  QCHECK(Has(Lines("combat_shieldgamma_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig gammaOnly;
  gammaOnly.shieldgammaGamma = {true, nevr_quest::Source::kFile};
  gammaOnly.shieldgammaTubeMirror = {false, nevr_quest::Source::kFile};
  combat::SetConfig(gammaOnly);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_shieldgamma_install")[0].text, "\"gamma\":1,\"tube_mirror\":0"));
  Done();
}

// The pfx gate and the event-effect decision read their keys from the resolved section (#2188): on with the defaults, off by the file's word.
void ThePfxRowsReadTheirKeysFromTheResolvedSection() {
  Process(bv::Variant::kCombat);
  combat::SetConfig(nevr_quest::CombatConfig{});
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_install")[0].text, "pfx=installed pfxtev=installed"));
  QCHECK(Has(Lines("combat_pfx_install")[0].text, "\"state\":\"on\""));
  QCHECK(Has(Lines("combat_pfxtev_install")[0].text, "\"state\":\"on\""));
  Done();
  // Each row reads its own key: pfx off alone, then pfxtev off alone.
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig pfxOff;
  pfxOff.pfx = {false, nevr_quest::Source::kFile};
  combat::SetConfig(pfxOff);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_install")[0].text, "pfx=off_config pfxtev=installed"));
  QCHECK(Has(Lines("combat_pfx_install")[0].text, "\"reason\":\"off_config\""));
  QCHECK(Has(Lines("combat_pfxtev_install")[0].text, "\"state\":\"on\""));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig tevOff;
  tevOff.pfxtev = {false, nevr_quest::Source::kFile};
  combat::SetConfig(tevOff);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_install")[0].text, "pfx=installed pfxtev=off_config"));
  QCHECK(Has(Lines("combat_pfx_install")[0].text, "\"state\":\"on\""));
  QCHECK(Has(Lines("combat_pfxtev_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  // A plain release installs neither, and neither writes a line.
  Process(bv::Variant::kNone);
  combat::InstallNormal();
  QCHECK(Lines("combat_pfx_install").empty() && Lines("combat_pfxtev_install").empty());
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
  TheShippedTableHoldsHeapsThenMemsysEarlyAndTheTickThenTheRefreshRateNormal();
  TheRealStepsConsultTheGate();
  ANormalStepThatNeverRanIsRecordedAsNotStartedOnce();
  TheConfigLineNamesEachKeyItsValueAndItsSource();
  TheRefreshRateRowReadsItsThreeKeysFromTheResolvedSection();
  TheHudmeterAndShieldgammaRowsReadTheirKeysFromTheResolvedSection();
  ThePfxRowsReadTheirKeysFromTheResolvedSection();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("combat_install_test: all checks passed");
  return 0;
}
