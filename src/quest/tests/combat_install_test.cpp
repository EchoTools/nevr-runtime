// Host test for the combat area's two install steps (#2171, docs/adr/0046 rules 5 and 6): InstallEarly and InstallNormal each run one
// registration table in its own order, only when `combat::Active()` says combat runs in this process, and each writes one
// `combat_install` aggregate line (phase, variant, active, each module and its result). The table is injected here (RunPhase), so the
// rules are tested before any module has moved in: the order, the skip of a module with no step in the phase, one line per phase, a
// failure counted and named, a plain release that installs nothing and says so once, and the `not_started` record of a normal step
// that never got its moment. LogConfig writes the `combat_config` line.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/bvh.h"
#include "quest/combat/combat_install.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "hook_log.h"
#include "quest/combat/boosterpool.h"
#include "quest/combat/combat_active.h"
#include "quest/combat/aibots.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/custctx.h"
#include "quest/combat/decals.h"
#include "quest/combat/droneguard.h"
#include "quest/combat/dronekill.h"
#include "quest/combat/dronemuzzle.h"
#include "quest/combat/ec.h"
#include "quest/combat/ecfx.h"
#include "quest/combat/fxtrail.h"
#include "quest/combat/gunconnect.h"
#include "quest/combat/headset_gates.h"
#include "quest/combat/weapon_arm.h"
#include "quest/combat/heaps.h"
#include "quest/combat/impactsnd.h"
#include "quest/combat/instmodel.h"
#include "quest/combat/levelname.h"
#include "quest/combat/memsys.h"
#include "quest/combat/peerclamp.h"
#include "quest/combat/wallfire.h"
#include "quest/combat/meshstep.h"
#include "quest/combat/powerboost.h"
#include "quest/combat/shaderstep.h"
#include "quest/combat/tablestep.h"
#include "quest/combat/voipdrop.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

// sendguard's table row reads the sentinel's files folder (the plugin's old switch file); the test names a folder that does not exist.
namespace sentinel {
const char* FilesDir() { return "/nonexistent-nevr-files"; }
}  // namespace sentinel

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

// The `combat_install_module` records that the phase of `phaseLine` wrote (the ones between the phase line before it and it), as
// `"results":"<module>=<status> ..."` in the table's order, so a check reads every module's outcome from the log.
std::string ResultsOf(const Line& phaseLine) {
  std::size_t at = g_lines.size();
  for (std::size_t i = 0; i < g_lines.size(); ++i) {
    if (g_lines[i].text == phaseLine.text) {
      at = i;
      break;
    }
  }
  const auto field = [](const std::string& t, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    const std::size_t b = t.find(k);
    if (b == std::string::npos) return std::string();
    const std::size_t e = t.find('"', b + k.size());
    return t.substr(b + k.size(), e - b - k.size());
  };
  std::vector<std::string> parts;
  for (std::size_t i = at; i-- > 0;) {
    const std::string& t = g_lines[i].text;
    if (Has(t, "\"event\":\"combat_install\"")) break;  // the phase line before: its records are not this phase's
    if (!Has(t, "\"event\":\"combat_install_module\"")) continue;  // a module's own lines sit between the records
    parts.insert(parts.begin(), field(t, "module") + "=" + field(t, "status"));
  }
  std::string out = "\"results\":\"";
  for (std::size_t i = 0; i < parts.size(); ++i) out += (i == 0 ? "" : " ") + parts[i];
  return out + "\"";
}

// No sentinel line of this process was cut: the writer ends a line that does not fit its 768 bytes with `"truncated":true`.
bool NoLineIsCut() {
  for (const Line& l : g_lines) {
    if (Has(l.text, "\"truncated\":true")) return false;
  }
  return true;
}

std::vector<std::string> g_calls;
combat::ModuleResult Ok() noexcept { return {true, "installed"}; }
combat::ModuleResult Refused() noexcept { return {false, "build_id_mismatch"}; }
combat::ModuleResult EarlyA() noexcept { g_calls.emplace_back("a_early"); return Ok(); }
combat::ModuleResult NormalA() noexcept { g_calls.emplace_back("a_normal"); return Ok(); }
combat::ModuleResult NormalImpact() noexcept { g_calls.emplace_back("impactsnd"); return Ok(); }
combat::ModuleResult NormalDecals() noexcept { g_calls.emplace_back("decals"); return Ok(); }
combat::ModuleResult NormalFails() noexcept { g_calls.emplace_back("fails"); return Refused(); }
combat::ModuleResult NormalOff() noexcept { g_calls.emplace_back("off"); return {false, "off_config", true}; }
void EndA() noexcept { g_calls.emplace_back("a_end"); }
void EndB() noexcept { g_calls.emplace_back("b_end"); }

// Combat runs in this process (a combat APK) or does not (a plain release).
void Process(bv::Variant variant) {
  combat::ResetForTest();
  combat::ResetInstallForTest();
  // A new process has an empty tick table: every normal step registers the refresh-rate rule's callback again, and the table holds 16 (#2184's
  // rows added the runs that filled it).
  nevr_quest::combat::tick::ResetForTest();
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
    QCHECK(ResultsOf(l) == "\"results\":\"alpha=installed impactsnd=installed decals=installed\"");
    QCHECK(Has(l.text, "\"msg\":\"3 of 3 combat modules installed in this phase\""));
  }
  Done();
}

// A module switched off by its key is counted apart: neither installed nor failed, the line stays Info, and the message says so.
void AModuleSwitchedOffByItsKeyIsCountedApart() {
  Process(bv::Variant::kCombat);
  const combat::Module table[] = {{"alpha", nullptr, &NormalA}, {"switched", nullptr, &NormalOff}};
  const combat::Outcome o = combat::RunPhase(combat::Phase::kNormal, table, 2);
  QCHECK(o.ran && o.modules == 2 && o.installed == 1 && o.failed == 0 && o.off == 1);
  std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(lines[0].text, "\"modules\":2,\"installed\":1,\"failed\":0,\"off\":1,"));
    QCHECK(ResultsOf(lines[0]) == "\"results\":\"alpha=installed switched=off_config\"");
    QCHECK(Has(lines[0].text, "\"msg\":\"1 of 2 combat modules installed in this phase; 1 off by its config key or with nothing to act on\""));
  }
  // Every module off: nothing was installed, and the line says 0 of 1, not 1 of 1.
  Process(bv::Variant::kCombat);
  const combat::Module only[] = {{"switched", nullptr, &NormalOff}};
  const combat::Outcome none = combat::RunPhase(combat::Phase::kNormal, only, 1);
  QCHECK(none.modules == 1 && none.installed == 0 && none.failed == 0 && none.off == 1);
  lines = Lines("combat_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kInfo);
  if (lines.size() == 1U) QCHECK(Has(lines[0].text, "\"msg\":\"0 of 1 combat modules installed in this phase; 1 off by its config key or with nothing to act on\""));
  Done();
}

// The end of the process: each module's end step runs in table order, once combat runs here and its normal step ran; never before the step
// ran, and never in a plain release.
void TheEndOfTheProcessRunsEachModulesEndStep() {
  const combat::Module table[] = {{"a", nullptr, &NormalA, nullptr, nullptr, &EndA}, {"none", nullptr, &NormalDecals}, {"b", nullptr, &NormalImpact, nullptr, nullptr, &EndB}};
  Process(bv::Variant::kCombat);
  combat::RunEnd(table, 3);
  QCHECK(g_calls.empty());  // the normal step has not run
  combat::RunPhase(combat::Phase::kNormal, table, 3);
  g_calls.clear();
  combat::RunEnd(table, 3);
  QCHECK((g_calls == std::vector<std::string>{"a_end", "b_end"}));
  Process(bv::Variant::kNone);
  combat::RunPhase(combat::Phase::kNormal, table, 3);
  combat::RunEnd(table, 3);
  QCHECK(g_calls.empty());
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
    QCHECK(ResultsOf(lines[0]) == "\"results\":\"first=installed broken=build_id_mismatch last=installed\"");
    const std::vector<Line> records = Lines("combat_install_module");
    QCHECK(records.size() == 3U);
    if (records.size() == 3U) {
      QCHECK(records[0].level == sentinel::LogLevel::kInfo && Has(records[0].text, "\"outcome\":\"installed\""));
      QCHECK(records[1].level == sentinel::LogLevel::kWarn &&
             Has(records[1].text, "\"phase\":\"normal\",\"module\":\"broken\",\"outcome\":\"failed\",\"status\":\"build_id_mismatch\""));
    }
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

// A table too long for one line to name every module (14 names of 26 bytes, past what one 768-byte sentinel line holds beside the counts and
// the message): every module's outcome is a record of its own, whole, and the phase line is whole, with exact counts and its message.
void ALongTableWritesEveryModuleRecordAndAWholePhaseLine() {
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
    QCHECK(Has(lines[0].text, "\"msg\":\"14 of 14 combat modules installed in this phase\"}"));
  }
  const std::vector<Line> records = Lines("combat_install_module");
  QCHECK(records.size() == 14U);
  for (std::size_t i = 0; i < records.size(); ++i) {
    char want[192];
    std::snprintf(want, sizeof(want),
                  "\"event\":\"combat_install_module\",\"phase\":\"normal\",\"module\":\"module_with_a_long_name_%02zu\",\"outcome\":\"installed\","
                  "\"status\":\"installed\"}",
                  i + 1);
    QCHECK(records[i].level == sentinel::LogLevel::kInfo && Has(records[i].text, want));
  }
  QCHECK(NoLineIsCut());
  Done();
}

// The shipped table's rows: heaps then memsys in the early phase; the tick, the rule, the four table steps with impactsnd and its write-switch row
// after the first and decals with its two flip-switch rows after the last, ec, ecfx, dronekill, dronemuzzle and powerboost, gunconnect, aibots and weapon_arm (#2185), the gates, bvh with
// its two switch rows, wallfire, sendguard, pfx, pfxtev, sensorwd and music in the normal phase. Rows are found by name, so a module that moves in
// adds its row without
// renumbering this test; the order the plugin installed them in is checked between the named rows.
void TheShippedTableRowsAreWiredToTheirSteps() {
  std::size_t count = 0;
  const combat::Module* table = combat::ShippedModules(&count);
  QCHECK(count == 44);
  const auto at = [&](const char* name) -> std::size_t {
    for (std::size_t i = 0; i < count; ++i) {
      if (std::strcmp(table[i].name, name) == 0) return i;
    }
    return count;
  };
  QCHECK(count >= 2 && std::strcmp(table[0].name, "heaps") == 0 && table[0].early != nullptr && table[0].normal == nullptr);
  QCHECK(count >= 2 && std::strcmp(table[1].name, "memsys") == 0 && table[1].early != nullptr && table[1].normal == nullptr);
  const std::size_t gates = at("gates"), bvhRow = at("bvh"), bvhGate = at("bvh_gate"), bvhTouch = at("bvh_touch"), wallfire = at("wallfire"),
                    sendguard = at("sendguard"), pfx = at("pfx"), pfxtev = at("pfxtev"), sensorwd = at("sensorwd"), music = at("music");
  const std::size_t ec = at("ec"), ecfx = at("ecfx"), dronekill = at("dronekill"), dronemuzzle = at("dronemuzzle"), powerboost = at("powerboost");
  QCHECK(at("shaderstep") < ec && ec < ecfx && ecfx < dronekill && dronekill < dronemuzzle && dronemuzzle < powerboost && powerboost < gates);
  // gunconnect, aibots and weapon_arm (#2185): each registers a tick callback, so each comes after the tick's row, and before the gates, which the plugin installed after all its modules
  QCHECK(at("tick") < at("gunconnect") && powerboost < at("gunconnect") && at("gunconnect") < at("aibots") && at("aibots") < at("weapon_arm") && at("weapon_arm") < gates);
  QCHECK(table[at("gunconnect")].early == nullptr && table[at("gunconnect")].normal != nullptr && table[at("aibots")].normal != nullptr && table[at("weapon_arm")].normal != nullptr);
  QCHECK(gates < bvhRow && bvhRow < bvhGate && bvhGate < bvhTouch && bvhTouch < wallfire && wallfire < sendguard && sendguard < pfx && pfx < count);
  // sensorwd, then music (#2184): after the decisions, as the plugin installed them after the modules that move before them.
  QCHECK(pfx < pfxtev && pfxtev < sensorwd && sensorwd < music && music < count);
  if (music < count) {
    const combat::Module& sd = table[sensorwd];
    QCHECK(sd.switchKey != nullptr && std::strcmp(sd.switchKey, "combat.sensorwd") == 0 && sd.switchMember == &nevr_quest::CombatConfig::sensorwd);
    QCHECK(sd.early == nullptr && sd.normal != nullptr);
    const combat::Module& mu = table[music];
    QCHECK(mu.switchKey != nullptr && std::strcmp(mu.switchKey, "combat.music") == 0 && mu.switchMember == &nevr_quest::CombatConfig::music);
    QCHECK(mu.early == nullptr && mu.normal != nullptr);
  }
  if (pfx < count) {
    const combat::Module& sg = table[sendguard];
    QCHECK(sg.switchKey != nullptr && std::strcmp(sg.switchKey, "combat.sendguard") == 0 && sg.switchMember == &nevr_quest::CombatConfig::sendguard);
    QCHECK(sg.early == nullptr && sg.normal != nullptr && sg.end != nullptr);
    const combat::Module& b = table[bvhRow];
    QCHECK(b.switchKey != nullptr && std::strcmp(b.switchKey, "combat.bvh") == 0 && b.switchMember == &nevr_quest::CombatConfig::bvh);
    QCHECK(b.early == nullptr && b.normal != nullptr && b.end == nullptr);
    // bvh's two switches have no step of their own: their rows exist for their combat_config_switch records.
    QCHECK(table[bvhGate].early == nullptr && table[bvhGate].normal == nullptr && table[bvhGate].switchMember == &nevr_quest::CombatConfig::bvhGate);
    QCHECK(table[bvhTouch].early == nullptr && table[bvhTouch].normal == nullptr && table[bvhTouch].switchMember == &nevr_quest::CombatConfig::bvhTouch);
    const combat::Module& w = table[wallfire];
    QCHECK(w.switchKey != nullptr && std::strcmp(w.switchKey, "combat.wallfire") == 0 && w.switchMember == &nevr_quest::CombatConfig::wallfire);
    QCHECK(w.early == nullptr && w.normal != nullptr);
  }
  // The combat level answer's row sits after the tick's, before every module that reads it; hudmeter then shieldgamma (#2186) sit after the decisions and
  // before sensorwd, as the plugin installed them, and the shield colour's two switches are rows with no step right after it.
  const std::size_t scopeRow = at("scope"), hudmeter = at("hudmeter"), shieldgamma = at("shieldgamma"), gamma = at("shieldgamma_gamma"),
                    mirror = at("shieldgamma_tube_mirror");
  QCHECK(at("tick") < scopeRow && scopeRow < at("refresh_rate") && at("refresh_rate") < hudmeter && pfxtev < hudmeter && hudmeter < shieldgamma &&
         shieldgamma + 1 == gamma && gamma + 1 == mirror && mirror < sensorwd && sensorwd < count);
  if (mirror < count) {
    QCHECK(table[scopeRow].early == nullptr && table[scopeRow].normal != nullptr && table[scopeRow].switchKey == nullptr);
    const combat::Module& hm = table[hudmeter];
    QCHECK(hm.early == nullptr && hm.normal != nullptr && hm.end == nullptr);
    QCHECK(hm.switchKey != nullptr && std::strcmp(hm.switchKey, "combat.hudmeter") == 0 && hm.switchMember == &nevr_quest::CombatConfig::hudmeter);
    QCHECK(table[shieldgamma].early == nullptr && table[shieldgamma].normal != nullptr && table[shieldgamma].end == nullptr);
    const struct {
      std::size_t row;
      const char* key;
      nevr_quest::CombatSwitch nevr_quest::CombatConfig::* member;
    } wristRows[] = {{gamma, "combat.shieldgamma_gamma", &nevr_quest::CombatConfig::shieldgammaGamma},
                     {mirror, "combat.shieldgamma_tube_mirror", &nevr_quest::CombatConfig::shieldgammaTubeMirror}};
    for (const auto& wr : wristRows) {
      const combat::Module& m = table[wr.row];
      QCHECK(m.early == nullptr && m.normal == nullptr && m.end == nullptr && m.switchMember == wr.member && m.switchKey != nullptr &&
             std::strcmp(m.switchKey, wr.key) == 0);
    }
  }
  // impactsnd sits after meshstep and before tablestep, decals after shaderstep and before the gates (#2183); each switch of theirs beyond the first
  // is a row with no step, right after its module, and neither module has an end step.
  const std::size_t meshstep = at("meshstep"), impact = at("impactsnd"), impactWrite = at("impactsnd_write"), tablestep = at("tablestep"),
                    shaderstep = at("shaderstep"), decals = at("decals"), flipU = at("decals_flip_u"), flipV = at("decals_flip_v");
  QCHECK(meshstep < impact && impact + 1 == impactWrite && impactWrite < tablestep && shaderstep < decals && decals + 1 == flipU && flipU + 1 == flipV &&
         flipV < gates && gates < count);
  if (flipV < count) {
    const combat::Module& is = table[impact];
    QCHECK(is.early == nullptr && is.normal != nullptr && is.end == nullptr);
    QCHECK(is.switchKey != nullptr && std::strcmp(is.switchKey, "combat.impactsnd") == 0 && is.switchMember == &nevr_quest::CombatConfig::impactsnd);
    const combat::Module& dc = table[decals];
    QCHECK(dc.early == nullptr && dc.normal != nullptr && dc.end == nullptr);
    QCHECK(dc.switchKey != nullptr && std::strcmp(dc.switchKey, "combat.decals") == 0 && dc.switchMember == &nevr_quest::CombatConfig::decals);
    const struct {
      std::size_t row;
      const char* key;
      nevr_quest::CombatSwitch nevr_quest::CombatConfig::* member;
    } switchRows[] = {{impactWrite, "combat.impactsnd_write", &nevr_quest::CombatConfig::impactsndWrite},
                      {flipU, "combat.decals_flip_u", &nevr_quest::CombatConfig::decalsFlipU},
                      {flipV, "combat.decals_flip_v", &nevr_quest::CombatConfig::decalsFlipV}};
    for (const auto& sr : switchRows) {
      const combat::Module& m = table[sr.row];
      QCHECK(m.early == nullptr && m.normal == nullptr && m.end == nullptr && m.switchMember == sr.member && m.switchKey != nullptr &&
             std::strcmp(m.switchKey, sr.key) == 0);
    }
  }
}

// The real steps, with the table as it ships: heaps and memsys have an early step, in that order, and 32 modules have a normal one (the tick,
// the combat level answer, the refresh-rate rule, the four table steps, impactsnd, decals, ec, ecfx, dronekill, dronemuzzle, powerboost, gunconnect, aibots, weapon_arm, the gates,
// bvh, wallfire, sendguard, pfx, pfxtev, hudmeter, shieldgamma, sensorwd and music). No libr15 is loaded in this process, so the modules that
// hook refuse with their fixed token and each line names them; the rule and the two decisions install.
void TheShippedTableHoldsHeapsThenMemsysEarlyAndTheTickTheRefreshRateAndTheTableStepsNormal() {
  nevr_quest::combat::heaps::ResetForTest();
  nevr_quest::combat::memsys::ResetForTest();
  nevr_quest::combat::meshstep::ResetForTest();
  nevr_quest::combat::tablestep::ResetForTest();
  nevr_quest::combat::instmodel::ResetForTest();
  nevr_quest::combat::shaderstep::ResetForTest();
  nevr_quest::combat::levelname::ResetForTest();
  nevr_quest::combat::boosterpool::ResetForTest();
  Process(bv::Variant::kCombat);
  const combat::Outcome early = combat::InstallEarly();
  const combat::Outcome normal = combat::InstallNormal();
  QCHECK(early.ran && early.modules == 2 && early.installed == 0 && early.failed == 2 && early.off == 0);
  // The normal step runs the tick (combat_tick.h) first, the refresh-rate rule after it (#2172), the four resource table steps in the plugin's order
  // (#2177) with the impact sounds (#2183) after the first and the level decals after the last, the EC client, NEUTRON effects, dronekill,
  // dronemuzzle and POWER BOOST (#2187), the headset gates (#2180), the static BVH and the wall-fire check (#2182: the BVH is refused for want of a
  // libr15, and the wall-fire check for want of the BVH), sendguard (#2179), the pfx gate and the event-effect decision, which hook nothing (#2188),
  // then gunconnect, aibots and weapon_arm (#2185), levelname with the booster pool (#2181) and, last, the scanner-sphere watchdog and the combat music (#2184), on by default. On the host no libr15 is loaded, so the tick, the table
  // steps, the impact sounds, the EC client, dronekill, POWER BOOST (at its default stage 3), gunconnect, aibots, weapon_arm, the gates, the BVH, the wall-fire check, sendguard,
  // levelname, the watchdog and the music are refused and named (NEUTRON effects refuse without the client, dronemuzzle without dronekill's shot, the booster pool without levelname); the rule
  // installs (it registers its callback for a tick that may come later), and so do the two decisions; the level decals, on by default, find no
  // decals.bin and are off: counted apart, neither installed nor failed.
  QCHECK(normal.ran && normal.modules == 34 && normal.installed == 3 && normal.failed == 30 && normal.off == 1);
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 2U);
  if (lines.size() == 2U) {
    QCHECK(Has(lines[0].text, "\"phase\":\"early\"") && Has(lines[1].text, "\"phase\":\"normal\""));
    QCHECK(lines[0].level == sentinel::LogLevel::kWarn);
    QCHECK(ResultsOf(lines[0]) == "\"results\":\"heaps=module_not_loaded memsys=module_not_loaded\"");
    QCHECK(lines[1].level == sentinel::LogLevel::kWarn);  // the refused tick makes the line a Warning
    QCHECK(ResultsOf(lines[1]) ==
           std::string("\"results\":\"tick=module_not_loaded scope=tick_not_installed refresh_rate=installed meshstep=module_not_loaded impactsnd=module_not_loaded tablestep=module_not_loaded "
               "instmodel=module_not_loaded shaderstep=module_not_loaded decals=no_table ec=module_not_loaded ecfx=needs_ec dronekill=module_not_loaded "
               "dronemuzzle=needs_dronekill_shot powerboost=module_not_loaded gunconnect=module_not_loaded aibots=module_not_loaded weapon_arm=module_not_loaded gates=module_not_loaded bvh=module_not_loaded "
               "wallfire=bvh_not_installed sendguard=module_not_loaded fxtrail=scope_not_live voipdrop=module_not_loaded custctx=module_not_loaded droneguard=module_not_loaded "
               "peerclamp=scope_not_live pfx=installed pfxtev=installed hudmeter=scope_not_live shieldgamma=scope_not_live levelname=module_not_loaded boosterpool=needs_levelname sensorwd=module_not_loaded music=module_not_loaded\""));
    // one record per module with a step, early first: 2 early and 34 normal
    const std::vector<Line> records = Lines("combat_install_module");
    QCHECK(records.size() == 36U && Has(records[0].text, "\"phase\":\"early\"") && Has(records[1].text, "\"phase\":\"early\"") &&
           Has(records[2].text, "\"phase\":\"normal\""));
    // an installed or off module's record is Info, a refused one's a Warning, and its outcome says which
    for (const Line& r : records) {
      const bool failed = Has(r.text, "\"outcome\":\"failed\"");
      QCHECK(failed || Has(r.text, "\"outcome\":\"installed\"") || Has(r.text, "\"outcome\":\"off\""));
      QCHECK(r.level == (failed ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo));
    }
    QCHECK(Has(records.back().text, "\"module\":\"music\",\"outcome\":\"failed\",\"status\":\"module_not_loaded\""));
    QCHECK(Has(Lines("combat_install_module")[10].text, "\"module\":\"decals\",\"outcome\":\"off\",\"status\":\"no_table\""));
    QCHECK(NoLineIsCut());
    // the phase line ends with its message, uncut
    QCHECK(Has(lines[1].text, "1 off by its config key or with nothing to act on; the others keep the game's own behaviour\"}"));
  }
  Done();
}

// The shipped rows switched off by the configuration the constructor's step handed over: each answers off, and the phase line counts it
// apart (neither installed nor failed), at Info, with the message saying so. heaps off and memsys on: heaps is off, memsys (no libr15 here)
// fails; sendguard off in the normal phase: the tick, the table steps, the impact sounds, the gates, the BVH, the wall-fire check, the watchdog
// and the music still fail (no libr15), the level decals are off for want of a decals.bin, and the rule, pfx and pfxtev install.
void AShippedRowSwitchedOffByTheConfigIsCountedAsOff() {
  nevr_quest::combat::heaps::ResetForTest();
  nevr_quest::combat::memsys::ResetForTest();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig cfg;
  cfg.heaps = {false, nevr_quest::Source::kFile};
  cfg.sendguard = {false, nevr_quest::Source::kFile};
  combat::SetConfig(cfg);
  const combat::Outcome early = combat::InstallEarly();
  const combat::Outcome normal = combat::InstallNormal();
  QCHECK(early.modules == 2 && early.installed == 0 && early.failed == 1 && early.off == 1);
  QCHECK(normal.modules == 34 && normal.installed == 3 && normal.failed == 29 && normal.off == 2);
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 2U);
  if (lines.size() == 2U) {
    QCHECK(ResultsOf(lines[0]) == "\"results\":\"heaps=off memsys=module_not_loaded\"" && Has(lines[0].text, "\"off\":1"));
    QCHECK(Has(lines[0].text, "0 of 2 combat modules installed in this phase; 1 off by its config key or with nothing to act on; the others keep"));
    QCHECK(lines[1].level == sentinel::LogLevel::kWarn);  // the refused tick
    QCHECK(Has(ResultsOf(lines[1]), "sendguard=off_config fxtrail=scope_not_live voipdrop=module_not_loaded custctx=module_not_loaded droneguard=module_not_loaded peerclamp=scope_not_live pfx=installed pfxtev=installed"));
    QCHECK(Has(lines[1].text, "\"off\":2"));
    QCHECK(Has(lines[1].text, "3 of 34 combat modules installed in this phase; 2 off by their config key or with nothing to act on; the others keep"));
  }
  Done();
  // bvh and wallfire switched off (#2182): both are off, not failed, beside the decals (no decals.bin); the BVH's two switches of its own have no step
  // and are not counted.
  nevr_quest::combat::bvh::ResetForTest();
  nevr_quest::combat::wallfire::ResetForTest();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig noBvh;
  noBvh.bvh = {false, nevr_quest::Source::kFile};
  noBvh.wallfire = {false, nevr_quest::Source::kFile};
  combat::SetConfig(noBvh);
  const combat::Outcome off2 = combat::InstallNormal();
  QCHECK(off2.modules == 34 && off2.installed == 3 && off2.failed == 28 && off2.off == 3);
  const std::vector<Line> offLines = Lines("combat_install");
  QCHECK(offLines.size() == 1U);
  if (offLines.size() == 1U) {
    QCHECK(Has(ResultsOf(offLines[0]), "bvh=off_config wallfire=off_config sendguard=module_not_loaded"));
    QCHECK(Has(offLines[0].text, "\"off\":3") && Has(offLines[0].text, "3 of 34 combat modules installed in this phase; 3 off by their config key or with nothing to act on"));
  }
  combat::SetConfig(nevr_quest::CombatConfig{});
  Done();
}

// The real steps in a plain release and in the arena APK (#2300): nothing installs, and the one line is the skip.
void TheRealStepsConsultTheGate() {
  for (const bv::Variant variant : {bv::Variant::kNone, bv::Variant::kArena}) {
    Process(variant);
    QCHECK(!combat::InstallEarly().ran);
    QCHECK(!combat::InstallNormal().ran);
    const std::vector<Line> lines = Lines("combat_install");
    QCHECK(lines.size() == 1U);
    QCHECK(lines.size() == 1U && Has(lines[0].text, "\"state\":\"skipped\""));
    QCHECK(Lines("combat_sendguard_install").empty() && Lines("combat_sendguard_stats").empty());  // no module of combat's ran or spoke
    QCHECK(Lines("combat_bvh_install").empty() && Lines("combat_wallfire_install").empty());
    // the EC client, NEUTRON effects, POWER BOOST and the drone modules (#2187) neither installed nor wrote a line of their own: no install
    // line, no words or page line, nothing under their event prefixes
    for (const char* prefix : {"\"event\":\"combat_ec", "\"event\":\"combat_powerboost", "\"event\":\"combat_drone"}) {
      for (const Line& l : g_lines) {
        if (Has(l.text, prefix)) std::fprintf(stderr, "%s: %s\n", bv::VariantName(variant), l.text.c_str());
        QCHECK(!Has(l.text, prefix));
      }
    }
    // the impact sounds and the level decals (#2183): neither installs, refuses or reads its table, so the arena APK keeps the game's own table and draws no decal
    QCHECK(Lines("combat_impactsnd_install").empty() && Lines("combat_impactsnd_refused").empty() && Lines("combat_decals_install").empty());
    combat::NoteProcessEnd();
    QCHECK(Lines("combat_sendguard_stats").empty());
    Done();
  }
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
  cfg.sendguard = {false, nevr_quest::Source::kFile};
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
    // The four resource table steps (#2177) are records of their own (combat_config_switch), not keys of this full line.
    QCHECK(!Has(lines[0].text, "meshstep") && !Has(lines[0].text, "tablestep") && !Has(lines[0].text, "sendguard") &&
           !Has(lines[0].text, "levelname"));  // levelname (#2181) too
  }
  g_lines.clear();
  cfg.heaps = {false, nevr_quest::Source::kFile};
  cfg.memsys = {true, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  const std::vector<Line> switches = Lines("combat_config");
  QCHECK(switches.size() == 1U);
  if (switches.size() == 1U) {
    QCHECK(Has(switches[0].text, "\"heaps\":0") && Has(switches[0].text, "\"heaps_source\":\"config\""));
    QCHECK(Has(switches[0].text, "\"memsys\":1") && Has(switches[0].text, "\"memsys_source\":\"config\""));
    QCHECK(Has(switches[0].text, "\"msg\":"));  // the line is not cut off: one sentinel line holds 768 bytes
  }
  // The four resource table steps (#2177) and levelname (#2181): one `combat_config_switch` record each, generated from the registration table. They are
  // on by default, each with its source; the file's word where it gave one.
  g_lines.clear();
  nevr_quest::CombatConfig tables;
  combat::LogConfig(tables);
  tables.meshstep = {false, nevr_quest::Source::kFile};
  tables.tablestep = {false, nevr_quest::Source::kFile};
  tables.levelname = {false, nevr_quest::Source::kFile};
  tables.hudmeter = {false, nevr_quest::Source::kFile};  // the wrist HUD's and the shield colour's records (#2186) are generated from their rows too
  tables.shieldgammaGamma = {true, nevr_quest::Source::kFile};
  tables.sendguard = {false, nevr_quest::Source::kFile};  // sendguard's record (#2179) is generated from its table row like the four
  combat::LogConfig(tables);
  const std::vector<Line> records = Lines("combat_config_switch");
  // per call, in the table's order: the table steps with impactsnd and its write key after meshstep and decals with its two flip keys after
  // shaderstep (#2183), ec, ecfx, dronekill and dronemuzzle (#2187), gunconnect, aibots and weapon_arm (#2185), bvh's three switches, wallfire's and sendguard's (#2182, #2179), the
  // levelname's (#2181), the watchdog's and the combat music's (#2184), then the two keys of the EC client and POWER BOOST that are not a boolean (#2187)
  const char* const order[] = {"meshstep",      "impactsnd",     "impactsnd_write", "tablestep", "instmodel", "shaderstep",
                               "decals",        "decals_flip_u", "decals_flip_v",   "ec",        "ecfx",      "dronekill",
                               "dronemuzzle",   "gunconnect",    "aibots",          "weapon_arm", "bvh",           "bvh_gate",        "bvh_touch", "wallfire",  "sendguard",
                               "fxtrail",       "voipdrop",      "custctx",         "droneguard", "droneguard_stage1", "peerclamp",
                               "hudmeter",      "shieldgamma_gamma", "shieldgamma_tube_mirror", "levelname", "sensorwd", "music"};
  constexpr std::size_t kSwitches = sizeof(order) / sizeof(order[0]);
  constexpr std::size_t kPerCall = kSwitches + 2;
  QCHECK(records.size() == 2 * kPerCall);
  const char* const kOnDefault = "\"on\":1,\"source\":\"default\"";
  const char* const kOffDefault = "\"on\":0,\"source\":\"default\"";
  const char* const kOffConfig = "\"on\":0,\"source\":\"config\"";
  const char* const kOnConfig = "\"on\":1,\"source\":\"config\"";
  const auto want = [&](std::size_t call, std::size_t i) {
    const std::string name = order[i];
    if (name == "decals_flip_u" || name == "decals_flip_v" || name == "droneguard_stage1") return kOffDefault;  // the flips and droneguard's second stage are off by default
    if (name == "shieldgamma_gamma") return call == 1 ? kOnConfig : kOffDefault;  // the shield gamma is a test switch: off by default
    if (call == 1 && (name == "meshstep" || name == "tablestep" || name == "sendguard" || name == "hudmeter" || name == "levelname")) return kOffConfig;
    return kOnDefault;
  };
  if (records.size() == 2 * kPerCall) {
    for (std::size_t i = 0; i < kSwitches; ++i) {
      for (std::size_t call = 0; call < 2; ++call) {
        const Line& line = records[call * kPerCall + i];
        QCHECK(line.level == sentinel::LogLevel::kInfo);
        QCHECK(line.text.substr(line.text.find("\"event\"")) == std::string("\"event\":\"combat_config_switch\",\"module\":\"") + order[i] + "\",\"key\":\"combat." + order[i] +
                                        "\"," + want(call, i) + "}");
        QCHECK(line.text.size() < 200);
      }
    }
    // the EC client and POWER BOOST (#2187): the words and the stage are records of their own, after the table's rows
    for (std::size_t call = 0; call < 2; ++call) {
      const auto at = [&](std::size_t k) { return records[call * kPerCall + k].text.substr(records[call * kPerCall + k].text.find("\"event\"")); };
      QCHECK(at(kSwitches) == "\"event\":\"combat_config_switch\",\"module\":\"ec\",\"key\":\"combat.ec_words\",\"on\":1,\"source\":\"default\"}");
      QCHECK(at(kSwitches + 1) == "\"event\":\"combat_config_switch\",\"module\":\"powerboost\",\"key\":\"combat.powerboost_stage\",\"stage\":3,\"on\":1,\"source\":\"default\"}");
    }
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
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "refresh_rate=installed"));
  QCHECK(Has(Lines("combat_refresh_rate_install")[0].text, "\"mode\":\"match\""));
  Done();
  // The file's word reaches the row: the rule off is the witness.
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig off;
  off.refreshRate = {false, nevr_quest::Source::kFile};
  combat::SetConfig(off);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "refresh_rate=witness"));
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

// levelname and the booster pool (#2181) are one feature behind `combat.levelname`: with the key off (the file's word) both are off and say so; with it
// on, the booster pool installs only when levelname did, and says `needs_levelname` when it did not.
void TheLevelNameRowsReadTheirSwitchAndTheBoosterNeedsLevelname() {
  nevr_quest::combat::levelname::ResetForTest();
  nevr_quest::combat::boosterpool::ResetForTest();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig off;
  off.levelname = {false, nevr_quest::Source::kFile};
  combat::SetConfig(off);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "levelname=off boosterpool=off"));
  // off is counted as off, not failed (#2181): each row's record says outcome off at Info, and no record of either row is a Warning or a failure
  for (const char* module : {"levelname", "boosterpool"}) {
    const std::string who = std::string("\"module\":\"") + module + "\"";
    std::size_t records = 0;
    for (const Line& l : Lines("combat_install_module")) {
      if (!Has(l.text, who)) continue;
      ++records;
      QCHECK(Has(l.text, "\"outcome\":\"off\"") && l.level == sentinel::LogLevel::kInfo);
    }
    QCHECK(records == 1U);
  }
  QCHECK(Has(Lines("combat_levelname_install")[0].text, "\"reason\":\"off_config\""));
  QCHECK(Has(Lines("combat_boosterpool_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  // On (the default): no libr15 here, so levelname is refused and the booster pool does not try.
  Process(bv::Variant::kCombat);
  combat::SetConfig(nevr_quest::CombatConfig{});
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "levelname=module_not_loaded boosterpool=needs_levelname"));
  QCHECK(Has(Lines("combat_boosterpool_install")[0].text, "\"reason\":\"needs_levelname\""));
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
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "hudmeter=scope_not_live shieldgamma=scope_not_live"));
  QCHECK(Lines("combat_hudmeter_install").size() == 1U && Has(Lines("combat_hudmeter_install")[0].text, "\"result\":\"refused\""));
  QCHECK(Has(Lines("combat_shieldgamma_install")[0].text, "\"gamma\":0,\"tube_mirror\":1"));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig off;
  off.hudmeter = {false, nevr_quest::Source::kFile};
  off.shieldgammaTubeMirror = {false, nevr_quest::Source::kFile};  // the gamma is off too: both false
  off.fxtrail = {false, nevr_quest::Source::kFile};                // the crash guards that read the scope (#2178) are readers too
  off.peerclamp = {false, nevr_quest::Source::kFile};
  off.custctx = {false, nevr_quest::Source::kFile};
  combat::SetConfig(off);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "hudmeter=off shieldgamma=off"));
  // every reader of the scope is off: the scope's row is off too, so arena pays no look for nothing (and it is off, not failed)
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "scope=off_config"));
  QCHECK(Lines("combat_scope_install").empty());
  bool scopeRecordIsOff = false;
  for (const Line& r : Lines("combat_install_module")) scopeRecordIsOff = scopeRecordIsOff || Has(r.text, "\"module\":\"scope\",\"outcome\":\"off\",\"status\":\"off_config\"");
  QCHECK(scopeRecordIsOff);  // off is counted apart: neither installed nor failed
  QCHECK(Has(Lines("combat_hudmeter_install")[0].text, "\"reason\":\"off_config\""));
  QCHECK(Has(Lines("combat_shieldgamma_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig gammaOnly;
  gammaOnly.hudmeter = {false, nevr_quest::Source::kFile};  // the gamma alone is a reader
  gammaOnly.shieldgammaGamma = {true, nevr_quest::Source::kFile};
  gammaOnly.shieldgammaTubeMirror = {false, nevr_quest::Source::kFile};
  combat::SetConfig(gammaOnly);
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_shieldgamma_install")[0].text, "\"gamma\":1,\"tube_mirror\":0"));
  QCHECK(!Has(ResultsOf(Lines("combat_install")[0]), "scope=off_config"));  // one reader on keeps the scope's row on
  Done();
  // each single reader keeps the row on: the wrist HUD alone, the tube mirror alone
  for (int only = 0; only < 2; ++only) {
    Process(bv::Variant::kCombat);
    nevr_quest::CombatConfig one;
    one.hudmeter = {only == 0, nevr_quest::Source::kFile};
    one.shieldgammaTubeMirror = {only == 1, nevr_quest::Source::kFile};
    combat::SetConfig(one);
    combat::InstallNormal();
    QCHECK(!Has(ResultsOf(Lines("combat_install")[0]), "scope=off_config"));
    Done();
  }
}

// The headset gates' keys have a line of their own (a log line is one 768-byte buffer): the defaults, each from the default; a key the file gave.
void TheGatesKeysHaveALineOfTheirOwn() {
  Process(bv::Variant::kCombat);
  sentinel::SetLogSink(&Capture);
  nevr_quest::CombatConfig cfg;
  QCHECK(cfg.gatePerm.on && cfg.gateBots.on && cfg.gateLink.on && cfg.gateLinkLibs.source == nevr_quest::Source::kAbsent);
  combat::LogConfig(cfg);
  cfg.gateBots = {false, nevr_quest::Source::kFile};
  cfg.gateLinkLibs.source = nevr_quest::Source::kFile;
  combat::LogConfig(cfg);
  const std::vector<Line> lines = Lines("combat_config_gates");
  QCHECK(lines.size() == 2U);
  if (lines.size() == 2U) {
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(lines[0].text, "\"gate_perm\":1") && Has(lines[0].text, "\"gate_perm_source\":\"default\""));
    QCHECK(Has(lines[0].text, "\"gate_bots\":1") && Has(lines[0].text, "\"gate_bots_source\":\"default\""));
    QCHECK(Has(lines[0].text, "\"gate_link\":1") && Has(lines[0].text, "\"gate_link_source\":\"default\""));
    QCHECK(Has(lines[0].text, "\"gate_link_libs_source\":\"default\""));
    QCHECK(Has(lines[1].text, "\"gate_bots\":0") && Has(lines[1].text, "\"gate_bots_source\":\"config\""));
    QCHECK(Has(lines[1].text, "\"gate_link_libs_source\":\"config\""));
    QCHECK(Has(lines[0].text, "\"msg\":"));  // no line is cut: the last field is there
  }
  // `combat_config` itself still ends in its message (the buffer holds it).
  const std::vector<Line> main = Lines("combat_config");
  QCHECK(main.size() == 2U && Has(main[0].text, "\"msg\":"));
  Done();
}

// The gates row reads its keys from the resolved section: all three off is the module's `off`, and a configured policy text reaches the module.
void TheGatesRowReadsItsKeysFromTheResolvedSection() {
  Process(bv::Variant::kCombat);
  nevr_quest::combat::headset_gates::ResetForTest();  // the shipped-table test above ran the module once
  nevr_quest::CombatConfig off;
  off.gatePerm = {false, nevr_quest::Source::kFile};
  off.gateBots = {false, nevr_quest::Source::kFile};
  off.gateLink = {false, nevr_quest::Source::kFile};
  combat::SetConfig(off);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "gates=off"));
  Done();

  // A libr15 holding the pinned words and slots, and a policy text the file gave: the menu gate's policy line says "config" and carries the text.
  Process(bv::Variant::kCombat);
  combat_test::Image image;
  namespace hg = nevr_quest::combat::headset_gates;
  image.Build({{hg::kPartySymbol, hg::kPins.partySlot, false, false, hg::kPins.partyFn},
               {hg::kHeadsetSymbol, hg::kPins.headsetSlot, false, false, hg::kPins.headsetFn}});
  for (const hg::Word& w : hg::kWords) image.Put<std::uint32_t>(w.vaddr, w.word);
  *image.Slot(hg::kPins.partySlot) = reinterpret_cast<void*>(image.Base() + hg::kPins.partyFn);
  *image.Slot(hg::kPins.headsetSlot) = reinterpret_cast<void*>(image.Base() + hg::kPins.headsetFn);
  static const combat_test::Image* g_image = nullptr;
  g_image = &image;
  hg::ResetForTest();
  hg::SetImageLookupForTest([](const char*, sentinel::ElfImage* out) {
    *out = g_image->Elf();
    return true;
  });
  nevr_quest::CombatConfig cfg;
  std::strcpy(cfg.gateLinkLibs.text, "0e4384a5 1703a89b");
  cfg.gateLinkLibs.source = nevr_quest::Source::kFile;
  combat::SetConfig(cfg);
  combat::InstallNormal();
  const std::vector<Line> policy = Lines("combat_gates_link_policy");
  QCHECK(policy.size() == 1U && Has(policy[0].text, "\"libs\":\"0e4384a5 1703a89b\"") && Has(policy[0].text, "\"source\":\"config\""));
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "gates=installed"));
  hg::ResetForTest();

  hg::ResetForTest();
  image.Drop();
  Done();
}

// The pfx gate and the event-effect decision read their keys from the resolved section (#2188): on with the defaults, off by the file's word.
void ThePfxRowsReadTheirKeysFromTheResolvedSection() {
  Process(bv::Variant::kCombat);
  combat::SetConfig(nevr_quest::CombatConfig{});
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "pfx=installed pfxtev=installed"));
  QCHECK(Has(Lines("combat_pfx_install")[0].text, "\"state\":\"on\""));
  QCHECK(Has(Lines("combat_pfxtev_install")[0].text, "\"state\":\"on\""));
  Done();
  // Each row reads its own key: pfx off alone, then pfxtev off alone.
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig pfxOff;
  pfxOff.pfx = {false, nevr_quest::Source::kFile};
  combat::SetConfig(pfxOff);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "pfx=off_config pfxtev=installed"));
  QCHECK(Has(Lines("combat_pfx_install")[0].text, "\"reason\":\"off_config\""));
  QCHECK(Has(Lines("combat_pfxtev_install")[0].text, "\"state\":\"on\""));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig tevOff;
  tevOff.pfxtev = {false, nevr_quest::Source::kFile};
  combat::SetConfig(tevOff);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "pfx=installed pfxtev=off_config"));
  QCHECK(Has(Lines("combat_pfx_install")[0].text, "\"state\":\"on\""));
  QCHECK(Has(Lines("combat_pfxtev_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  // A plain release installs neither, and neither writes a line.
  Process(bv::Variant::kNone);
  combat::InstallNormal();
  QCHECK(Lines("combat_pfx_install").empty() && Lines("combat_pfxtev_install").empty());
  Done();
}

// The three modules of #2185 have a record of their own for their switch, generated from their table rows, each on from the default.
void TheThreeSwitchesAreReportedOneRecordEachFromTheTable() {
  g_lines.clear();
  sentinel::SetLogSink(&Capture);
  nevr_quest::CombatConfig cfg;
  combat::LogConfig(cfg);
  cfg.gunconnect = {false, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  const std::vector<Line> lines = Lines("combat_config_switch");
  struct Want {
    const char* module;
    const char* key;
  };
  const Want want[] = {{"gunconnect", "combat.gunconnect"}, {"aibots", "combat.aibots"}, {"weapon_arm", "combat.weapon_arm"}};
  for (const Want& w : want) {
    int n = 0;
    for (const Line& l : lines) {
      n += Has(l.text, std::string("\"module\":\"") + w.module + "\",\"key\":\"" + w.key + "\"") ? 1 : 0;
    }
    QCHECK(n == 2);
  }
  QCHECK(lines.size() >= 6U);
  for (const Line& l : lines) {
    if (Has(l.text, "\"module\":\"gunconnect\"")) {
      QCHECK(Has(l.text, "\"on\":1,\"source\":\"default\"") || Has(l.text, "\"on\":0,\"source\":\"config\""));
    }
    if (Has(l.text, "\"module\":\"aibots\"") || Has(l.text, "\"module\":\"weapon_arm\"")) QCHECK(Has(l.text, "\"on\":1,\"source\":\"default\""));
  }
  Done();
}

// Each of the three rows reads its own key, and aibots also its scenario text: gunconnect off alone, then aibots off alone, then weapon_arm off alone (the level decals are off too: no decals.bin on the host).
void TheGunAndBotRowsReadTheirKeysFromTheResolvedSection() {
  const char* const results[3] = {"gunconnect=off_config aibots=module_not_loaded weapon_arm=module_not_loaded", "gunconnect=module_not_loaded aibots=off_config weapon_arm=module_not_loaded",
                                  "gunconnect=module_not_loaded aibots=module_not_loaded weapon_arm=off_config"};
  for (int i = 0; i < 3; ++i) {
    Process(bv::Variant::kCombat);
    nevr_quest::combat::gunconnect::ResetForTest();
    nevr_quest::combat::aibots::ResetForTest();
    nevr_quest::combat::weapon_arm::ResetForTest();
    nevr_quest::CombatConfig cfg;
    (i == 0 ? cfg.gunconnect : i == 1 ? cfg.aibots : cfg.weaponArm) = {false, nevr_quest::Source::kFile};
    combat::SetConfig(cfg);
    combat::InstallNormal();
    QCHECK(Has(ResultsOf(Lines("combat_install")[0]), results[i]));
    QCHECK(Has(Lines("combat_install")[0].text, "\"off\":2"));
    Done();
  }
}

// aibots takes whether this is a development build from the gate's answer (combat::DevBuild): the plugin's request file is warned about in a release build only.
void TheBotsRowTellsTheModuleWhetherThisIsADevelopmentBuild() {
  char dir[] = "/tmp/combat-install-XXXXXX";
  QCHECK(mkdtemp(dir) != nullptr);
  const std::string file = std::string(dir) + "/aibots.req";
  std::FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  for (const bool dev : {false, true}) {
    Process(bv::Variant::kCombat);
    combat::ResetForTest();
    combat::SetVariant(bv::Variant::kCombat);
    combat::Resolve(dev, /*forceAsked=*/false);
    QCHECK(combat::DevBuild() == dev);
    nevr_quest::combat::aibots::ResetForTest();
    nevr_quest::combat::aibots::SetLegacyDirForTest(dir);
    g_lines.clear();
    combat::InstallNormal();
    QCHECK(Lines("combat_aibots_legacy_switch").size() == (dev ? 0U : 1U));
    nevr_quest::combat::aibots::SetLegacyDirForTest(nullptr);
    Done();
  }
  combat::ResetForTest();
  QCHECK(!combat::DevBuild());
  // The scenario text the file gave reaches the module: an unknown word is warned about once, a known one is not (the warning is said before the
  // library is looked at, so the host sees it).
  for (const char* word : {"bogus", "patrol", "off"}) {
    Process(bv::Variant::kCombat);
    nevr_quest::combat::aibots::ResetForTest();
    nevr_quest::CombatConfig cfg;
    cfg.aibotsScenario.source = nevr_quest::Source::kFile;
    std::snprintf(cfg.aibotsScenario.text, sizeof(cfg.aibotsScenario.text), "%s", word);
    combat::SetConfig(cfg);
    combat::InstallNormal();
    QCHECK(Lines("combat_aibots_scenario").size() == (std::strcmp(word, "bogus") == 0 ? 1U : 0U));
    Done();
  }
  Process(bv::Variant::kCombat);  // no text from the file: the default stands and nothing is warned about
  nevr_quest::combat::aibots::ResetForTest();
  combat::SetConfig(nevr_quest::CombatConfig{});
  combat::InstallNormal();
  QCHECK(Lines("combat_aibots_scenario").empty());
  Done();
  unlink(file.c_str());
  rmdir(dir);
}

// The five modules of #2187 switched off by their key are off, not failed: each alone, then all five. The phase line counts them in `off`
// and nowhere else, and names each one `off`.
void TheEcAndDroneModulesSwitchedOffByTheirKeyAreCountedOff() {
  struct Case {
    const char* name;
    void (*set)(nevr_quest::CombatConfig&);
  };
  const Case cases[] = {
      {"ec", [](nevr_quest::CombatConfig& c) { c.ec = {false, nevr_quest::Source::kFile}; }},
      {"ecfx", [](nevr_quest::CombatConfig& c) { c.ecfx = {false, nevr_quest::Source::kFile}; }},
      {"dronekill", [](nevr_quest::CombatConfig& c) { c.dronekill = {false, nevr_quest::Source::kFile}; }},
      {"dronemuzzle", [](nevr_quest::CombatConfig& c) { c.dronemuzzle = {false, nevr_quest::Source::kFile}; }},
      {"powerboost", [](nevr_quest::CombatConfig& c) { c.powerboostStage = {0, nevr_quest::Source::kFile}; }},
  };
  const auto reset = [] {
    nevr_quest::combat::ec::ResetForTest();
    nevr_quest::combat::ecfx::ResetForTest();
    nevr_quest::combat::dronekill::ResetForTest();
    nevr_quest::combat::dronemuzzle::ResetForTest();
    nevr_quest::combat::powerboost::ResetForTest();
  };
  nevr_quest::CombatConfig all;
  for (const Case& c : cases) {
    reset();
    Process(bv::Variant::kCombat);
    nevr_quest::CombatConfig one;
    c.set(one);
    c.set(all);
    combat::SetConfig(one);
    const combat::Outcome o = combat::InstallNormal();
    QCHECK(o.modules == 34 && o.off == 2 && o.installed == 3 && o.failed == 29);  // decals is off too: no decals.bin on the host
    if (o.off != 2 || o.installed != 3 || o.failed != 19)
      std::fprintf(stderr, "%s off by its key: installed=%zu failed=%zu off=%zu\n", c.name, o.installed, o.failed, o.off);
    const std::vector<Line> lines = Lines("combat_install");
    QCHECK(lines.size() == 1U && Has(ResultsOf(lines[0]), std::string(" ") + c.name + "=off ") && Has(lines[0].text, "\"off\":2"));
    Done();
  }
  reset();
  Process(bv::Variant::kCombat);
  combat::SetConfig(all);
  const combat::Outcome o = combat::InstallNormal();
  QCHECK(o.modules == 34 && o.off == 6 && o.installed == 3 && o.failed == 25);
  const std::vector<Line> lines = Lines("combat_install");
  QCHECK(lines.size() == 1U && Has(ResultsOf(lines[0]), "ec=off ecfx=off dronekill=off dronemuzzle=off powerboost=off") && Has(lines[0].text, "\"off\":6"));
  combat::SetConfig(nevr_quest::CombatConfig{});
  reset();
  Done();
}

// The EC client's rows read their keys from the resolved section (#2187): each off by the file's word, and the words the file gave reach the module
// (its words line says `config` and carries the mask; no libr15 is loaded, so the module refuses after reading them).
void TheEcRowsReadTheirKeysFromTheResolvedSection() {
  Process(bv::Variant::kCombat);
  nevr_quest::combat::ec::ResetForTest();
  nevr_quest::combat::ecfx::ResetForTest();
  nevr_quest::CombatConfig off;
  off.ec = {false, nevr_quest::Source::kFile};
  off.ecfx = {false, nevr_quest::Source::kFile};
  combat::SetConfig(off);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "ec=off ecfx=off"));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::combat::ec::ResetForTest();
  nevr_quest::combat::ecfx::ResetForTest();
  nevr_quest::CombatConfig words;
  std::strcpy(words.ecWords.text, "types=V noequip");
  words.ecWords.source = nevr_quest::Source::kFile;
  combat::SetConfig(words);
  combat::InstallNormal();
  const std::vector<Line> line = Lines("combat_ec_words");
  QCHECK(line.size() == 1U && Has(line[0].text, "\"source\":\"config\"") && Has(line[0].text, "\"types_mask\":\"0x0000000000000001\"") &&
         Has(line[0].text, "\"equip_store\":0"));
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "ec=module_not_loaded ecfx=needs_ec"));
  Done();
  // The defaults: the shipped words.
  Process(bv::Variant::kCombat);
  nevr_quest::combat::ec::ResetForTest();
  nevr_quest::combat::ecfx::ResetForTest();
  combat::SetConfig(nevr_quest::CombatConfig{});
  combat::InstallNormal();
  QCHECK(Has(Lines("combat_ec_words")[0].text, "\"source\":\"default\"") && Has(Lines("combat_ec_words")[0].text, "\"types_mask\":\"0x000000000000000f\""));
  nevr_quest::combat::ec::ResetForTest();
  nevr_quest::combat::ecfx::ResetForTest();
  Done();
}

// The watchdog and the combat music (#2184) are ON unless the file turns them off: the config line is one `combat_config_switch` record each, the rows read
// their own key, and a module that is on tries its install (here, with no libr15 loaded, it is refused and named).
void TheMusicAndSensorwdSwitchesAreOnByDefaultAndTheRowsReadThem() {
  Process(bv::Variant::kCombat);
  sentinel::SetLogSink(&Capture);
  nevr_quest::CombatConfig cfg;
  QCHECK(cfg.music.on && cfg.sensorwd.on && cfg.music.source == nevr_quest::Source::kAbsent);
  combat::LogConfig(cfg);
  cfg.sensorwd = {false, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  std::vector<Line> sw;  // the watchdog's and the music's records of the two calls, in the table's order
  for (const Line& l : Lines("combat_config_switch")) {
    if (Has(l.text, "\"module\":\"sensorwd\"") || Has(l.text, "\"module\":\"music\"")) sw.push_back(l);
  }
  QCHECK(sw.size() == 4U);
  if (sw.size() == 4U) {
    QCHECK(Has(sw[0].text, "\"module\":\"sensorwd\"") && Has(sw[0].text, "\"key\":\"combat.sensorwd\"") && Has(sw[0].text, "\"on\":1") &&
           Has(sw[0].text, "\"source\":\"default\""));
    QCHECK(Has(sw[1].text, "\"module\":\"music\"") && Has(sw[1].text, "\"key\":\"combat.music\"") && Has(sw[1].text, "\"on\":1") &&
           Has(sw[1].text, "\"source\":\"default\""));
    QCHECK(Has(sw[2].text, "\"module\":\"sensorwd\"") && Has(sw[2].text, "\"on\":0") && Has(sw[2].text, "\"source\":\"config\""));
    QCHECK(Has(sw[3].text, "\"module\":\"music\"") && Has(sw[3].text, "\"on\":1") && Has(sw[3].text, "\"source\":\"default\""));
    QCHECK(sw[0].level == sentinel::LogLevel::kInfo);
  }
  Done();
  // The rows: on by default (no libr15 here, so each is refused and named); music off alone, then sensorwd off alone, each reaching only its own module.
  Process(bv::Variant::kCombat);
  combat::SetConfig(nevr_quest::CombatConfig{});
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "sensorwd=module_not_loaded music=module_not_loaded"));
  QCHECK(Has(Lines("combat_music_install")[0].text, "\"reason\":\"module_not_loaded\""));
  QCHECK(Has(Lines("combat_sensorwd_install")[0].text, "\"reason\":\"module_not_loaded\""));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig musicOff;
  musicOff.music = {false, nevr_quest::Source::kFile};
  combat::SetConfig(musicOff);
  const combat::Outcome mOff = combat::InstallNormal();
  // off by its key: counted apart, not failed (the decals are off too, for want of a decals.bin)
  QCHECK(mOff.modules == 34 && mOff.off == 2 && mOff.installed == 3 && mOff.failed == 29);
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "sensorwd=module_not_loaded music=off_config"));
  QCHECK(Has(Lines("combat_install")[0].text, "\"off\":2"));
  QCHECK(Has(Lines("combat_install_module").back().text, "\"module\":\"music\",\"outcome\":\"off\",\"status\":\"off_config\""));
  QCHECK(Has(Lines("combat_music_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig sensorOff;
  sensorOff.sensorwd = {false, nevr_quest::Source::kFile};
  combat::SetConfig(sensorOff);
  const combat::Outcome sOff = combat::InstallNormal();
  QCHECK(sOff.modules == 34 && sOff.off == 2 && sOff.installed == 3 && sOff.failed == 29);
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "sensorwd=off_config music=module_not_loaded"));
  QCHECK(Has(Lines("combat_install")[0].text, "\"off\":2"));
  {
    const std::vector<Line> recs = Lines("combat_install_module");
    QCHECK(recs.size() >= 2U && Has(recs[recs.size() - 2].text, "\"module\":\"sensorwd\",\"outcome\":\"off\",\"status\":\"off_config\""));
  }
  QCHECK(Has(Lines("combat_sensorwd_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  // A plain release and the arena APK (#2300, combat does not run there) install neither and write neither line, on by default or not.
  for (const bv::Variant variant : {bv::Variant::kNone, bv::Variant::kArena}) {
    Process(variant);
    combat::SetConfig(nevr_quest::CombatConfig{});
    QCHECK(!combat::InstallNormal().ran);
    QCHECK(Lines("combat_music_install").empty() && Lines("combat_sensorwd_install").empty() && Lines("combat_install_module").empty());
    Done();
  }
}

// The impact sounds and the level decals (#2183) read their keys from the resolved section: impactsnd on by default and decals on by default (inert without a
// decals.bin, which a host has none of), each off by the file's word alone, and a `combat_config_switch` record for each key of the two rows.
void TheImpactsndAndDecalsRowsReadTheirKeysFromTheResolvedSection() {
  namespace impactsnd = nevr_quest::combat::impactsnd;
  impactsnd::ResetForTest();
  Process(bv::Variant::kCombat);
  combat::SetConfig(nevr_quest::CombatConfig{});
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "impactsnd=module_not_loaded") && Has(ResultsOf(Lines("combat_install")[0]), "decals=no_table"));  // no libr15 on the host, and no decals.bin
  QCHECK(Lines("combat_impactsnd_refused").size() == 1U);
  QCHECK(Lines("combat_decals_install").size() == 1U && Has(Lines("combat_decals_install")[0].text, "\"reason\":\"no_table\""));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig impactOff;
  impactOff.impactsnd = {false, nevr_quest::Source::kFile};
  combat::SetConfig(impactOff);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "impactsnd=off_config") && Has(ResultsOf(Lines("combat_install")[0]), "decals=no_table"));
  // a module off by its key is off, not failed: the phase line counts it apart (impactsnd by its key, decals for want of a decals.bin), and the
  // nineteen others are failed: no libr15 on the host, or already_ran for the rows this test does not reset
  QCHECK(Has(Lines("combat_install")[0].text, "\"modules\":34") && Has(Lines("combat_install")[0].text, "\"off\":2") &&
         Has(Lines("combat_install")[0].text, "\"failed\":29"));
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "ec=already_ran"));
  QCHECK(Lines("combat_impactsnd_refused").empty() && Has(Lines("combat_impactsnd_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig decalsOff;
  decalsOff.decals = {false, nevr_quest::Source::kFile};
  combat::SetConfig(decalsOff);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "impactsnd=module_not_loaded") && Has(ResultsOf(Lines("combat_install")[0]), "decals=off_config"));
  QCHECK(Has(Lines("combat_decals_install")[0].text, "\"reason\":\"off_config\""));
  Done();
  // The config switch records: one for each key a row names, with the value and where it came from.
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig cfg;
  cfg.impactsndWrite = {false, nevr_quest::Source::kFile};
  cfg.decalsFlipU = {true, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  const std::vector<Line> sw = Lines("combat_config_switch");
  auto find = [&sw](const char* key) -> std::string {
    for (const Line& l : sw) {
      if (Has(l.text, std::string("\"key\":\"") + key + "\"")) return l.text;
    }
    return std::string();
  };
  QCHECK(Has(find("combat.impactsnd"), "\"module\":\"impactsnd\"") && Has(find("combat.impactsnd"), "\"on\":1") && Has(find("combat.impactsnd"), "\"source\":\"default\""));
  QCHECK(Has(find("combat.impactsnd_write"), "\"on\":0") && Has(find("combat.impactsnd_write"), "\"source\":\"config\""));
  QCHECK(Has(find("combat.decals"), "\"module\":\"decals\"") && Has(find("combat.decals"), "\"on\":1") && Has(find("combat.decals"), "\"source\":\"default\""));
  QCHECK(Has(find("combat.decals_flip_u"), "\"on\":1") && Has(find("combat.decals_flip_u"), "\"source\":\"config\""));
  // The table's order: impactsnd before decals (decals' Shutdown handler chains above impactsnd's), both after the tick.
  std::size_t n = 0;
  const combat::Module* table = combat::ShippedModules(&n);
  std::size_t tick = n, impact = n, decals = n;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::strcmp(table[i].name, "tick") == 0) tick = i;
    if (std::strcmp(table[i].name, "impactsnd") == 0) impact = i;
    if (std::strcmp(table[i].name, "decals") == 0) decals = i;
  }
  QCHECK(tick < impact && impact < decals && decals < n);
  QCHECK(table[impact].early == nullptr && table[impact].normal != nullptr && table[decals].early == nullptr && table[decals].normal != nullptr);
  Done();
}

}  // namespace

// The static BVH's and the wall-fire check's rows read their keys from the resolved section (#2182): each switch the file turned off reaches its module, and each
// is one generated `combat_config_switch` record (bvh_gate and bvh_touch are rows with no step of their own). The default is on, as q15t-upstream has it.
void TheBvhRowsReadTheirKeysFromTheResolvedSection() {
  nevr_quest::combat::bvh::ResetForTest();
  nevr_quest::combat::wallfire::ResetForTest();
  Process(bv::Variant::kCombat);
  combat::SetConfig(nevr_quest::CombatConfig{});
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "bvh=module_not_loaded wallfire=bvh_not_installed"));
  Done();
  nevr_quest::combat::bvh::ResetForTest();
  nevr_quest::combat::wallfire::ResetForTest();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig off;
  off.bvh = {false, nevr_quest::Source::kFile};
  off.bvhGate = {false, nevr_quest::Source::kFile};
  off.bvhTouch = {false, nevr_quest::Source::kFile};
  off.wallfire = {false, nevr_quest::Source::kFile};
  combat::SetConfig(off);
  combat::InstallNormal();
  QCHECK(Has(ResultsOf(Lines("combat_install")[0]), "bvh=off_config wallfire=off_config"));
  Done();
  // The row hands the module each of its three keys as the file gave it: bvh on, its gate off, its touch hook off.
  nevr_quest::combat::bvh::ResetForTest();
  nevr_quest::combat::wallfire::ResetForTest();
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig split;
  split.bvhGate = {false, nevr_quest::Source::kFile};
  split.bvhTouch = {false, nevr_quest::Source::kFile};
  combat::SetConfig(split);
  combat::InstallNormal();
  {
    const nevr_quest::combat::bvh::Switches given = nevr_quest::combat::bvh::LastSwitchesForTest();
    QCHECK(given.bvh && !given.gate && !given.touch);
  }
  Done();
  // The records: the defaults, then what the file gave.
  Process(bv::Variant::kCombat);
  combat::LogConfig(nevr_quest::CombatConfig{});
  combat::LogConfig(off);
  const std::vector<Line> records = Lines("combat_config_switch");
  const auto find = [&](const char* module, std::size_t call) -> std::string {
    std::size_t seen = 0;
    for (const Line& l : records) {
      if (Has(l.text, std::string("\"module\":\"") + module + "\"")) {
        if (seen++ == call) return l.text.substr(l.text.find("\"event\""));
      }
    }
    return "";
  };
  const char* const modules[] = {"bvh", "bvh_gate", "bvh_touch", "wallfire"};
  // and four switches that differ in value and source, so a record read from another row's member shows
  nevr_quest::CombatConfig mixed;
  mixed.bvh = {false, nevr_quest::Source::kFile};
  mixed.bvhGate = {true, nevr_quest::Source::kFile};
  mixed.bvhTouch = {false, nevr_quest::Source::kAbsent};
  mixed.wallfire = {true, nevr_quest::Source::kAbsent};
  g_lines.clear();
  combat::LogConfig(mixed);
  const std::vector<Line> all = Lines("combat_config_switch");
  const char* const want[4] = {"\"module\":\"bvh\",\"key\":\"combat.bvh\",\"on\":0,\"source\":\"config\"",
                               "\"module\":\"bvh_gate\",\"key\":\"combat.bvh_gate\",\"on\":1,\"source\":\"config\"",
                               "\"module\":\"bvh_touch\",\"key\":\"combat.bvh_touch\",\"on\":0,\"source\":\"default\"",
                               "\"module\":\"wallfire\",\"key\":\"combat.wallfire\",\"on\":1,\"source\":\"default\""};
  for (const char* w : want) {
    std::size_t hits = 0;
    for (std::size_t i = 0; i < all.size(); ++i) hits += Has(all[i].text, w) ? 1 : 0;
    QCHECK(hits == 1);
  }
  for (const char* m : modules) {
    QCHECK(find(m, 0) == std::string("\"event\":\"combat_config_switch\",\"module\":\"") + m + "\",\"key\":\"combat." + m + "\",\"on\":1,\"source\":\"default\"}");
    QCHECK(find(m, 1) == std::string("\"event\":\"combat_config_switch\",\"module\":\"") + m + "\",\"key\":\"combat." + m + "\",\"on\":0,\"source\":\"config\"}");
  }
  Done();
}

// The crash and timing guards' rows (#2178): the plugin's order after sendguard and before the pfx decisions, a normal-phase step and an end-of-process step
// each (droneguard_stage1 has neither: it is a switch of droneguard's, a row so the switch is one generated `combat_config_switch` record), no early step.
void TheGuardRowsAreNormalPhaseRowsInThePluginsOrderEachWithAnEndStep() {
  std::size_t count = 0;
  const combat::Module* const table = combat::ShippedModules(&count);
  const auto at = [&](const char* name) -> std::size_t {
    for (std::size_t i = 0; i < count; ++i) {
      if (std::strcmp(table[i].name, name) == 0) return i;
    }
    return count;
  };
  const char* const names[] = {"fxtrail", "voipdrop", "custctx", "droneguard", "droneguard_stage1", "peerclamp"};
  const std::size_t first = at("fxtrail");
  QCHECK(first < count && at("sendguard") + 1 == first && first + 6 == at("pfx") && at("scope") < first);
  for (std::size_t i = 0; i < 6 && first + i < count; ++i) QCHECK(std::strcmp(table[first + i].name, names[i]) == 0);
  for (const char* name : {"fxtrail", "voipdrop", "custctx", "droneguard", "peerclamp"}) {
    const combat::Module& m = table[at(name)];
    QCHECK(m.early == nullptr && m.normal != nullptr && m.end != nullptr && m.switchKey != nullptr && m.switchMember != nullptr);
  }
  const combat::Module& stage1 = table[at("droneguard_stage1")];
  QCHECK(stage1.early == nullptr && stage1.normal == nullptr && stage1.end == nullptr);
  QCHECK(stage1.switchKey != nullptr && std::strcmp(stage1.switchKey, "combat.droneguard_stage1") == 0 &&
         stage1.switchMember == &nevr_quest::CombatConfig::droneguardStage1);
}

// Each guard's row reads its own switch: a key set to false is counted off, named `off_config`, and no other guard moves.
void EachGuardRowReadsItsOwnSwitch() {
  struct Case {
    const char* name;
    nevr_quest::CombatSwitch nevr_quest::CombatConfig::*member;
  };
  const Case cases[] = {{"fxtrail", &nevr_quest::CombatConfig::fxtrail},     {"voipdrop", &nevr_quest::CombatConfig::voipdrop},
                        {"custctx", &nevr_quest::CombatConfig::custctx},     {"droneguard", &nevr_quest::CombatConfig::droneguard},
                        {"peerclamp", &nevr_quest::CombatConfig::peerclamp}};
  Process(bv::Variant::kCombat);
  const combat::Outcome base = combat::InstallNormal();
  Done();
  for (const Case& c : cases) {
    Process(bv::Variant::kCombat);
    nevr_quest::CombatConfig cfg;
    cfg.*c.member = {false, nevr_quest::Source::kFile};
    combat::SetConfig(cfg);
    const combat::Outcome o = combat::InstallNormal();
    QCHECK(o.ran && o.off == base.off + 1 && o.failed == base.failed - 1 && o.modules == base.modules);
    const std::vector<Line> lines = Lines("combat_install");
    QCHECK(lines.size() == 1U && Has(ResultsOf(lines[0]), std::string(c.name) + "=off_config"));
    for (const Case& other : cases) {
      if (&other != &c && lines.size() == 1U) QCHECK(!Has(ResultsOf(lines[0]), std::string(other.name) + "=off_config"));
    }
    Done();
  }
  {  // the second stage of droneguard is its own key: it moves nothing of the first, and the guard says which stage it was asked for
    Process(bv::Variant::kCombat);
    nevr_quest::CombatConfig cfg;
    cfg.droneguardStage1 = {true, nevr_quest::Source::kFile};
    combat::SetConfig(cfg);
    const combat::Outcome o = combat::InstallNormal();
    QCHECK(o.ran && o.off == base.off && o.failed == base.failed);
    Done();
  }
}

// The guards that read the combat level answer (fxtrail, peerclamp, custctx's pool refusal) are readers of the scope's row: each alone keeps it on; voipdrop and
// droneguard act in every level and ask nothing of it, so with every reader off the row is off and arena pays no look for nothing.
void TheScopeRowStaysOnForEachGuardThatReadsItAndOffForTheOthers() {
  struct Only {
    const char* name;
    nevr_quest::CombatSwitch nevr_quest::CombatConfig::*member;
    bool reader;
  };
  const Only cases[] = {{"fxtrail", &nevr_quest::CombatConfig::fxtrail, true},     {"peerclamp", &nevr_quest::CombatConfig::peerclamp, true},
                        {"custctx", &nevr_quest::CombatConfig::custctx, true},     {"voipdrop", &nevr_quest::CombatConfig::voipdrop, false},
                        {"droneguard", &nevr_quest::CombatConfig::droneguard, false}};
  for (const Only& c : cases) {
    Process(bv::Variant::kCombat);
    nevr_quest::CombatConfig cfg;
    cfg.hudmeter = {false, nevr_quest::Source::kFile};
    cfg.shieldgammaTubeMirror = {false, nevr_quest::Source::kFile};  // the gamma is off by default: no other reader of the scope is on
    cfg.fxtrail = {false, nevr_quest::Source::kFile};
    cfg.peerclamp = {false, nevr_quest::Source::kFile};
    cfg.custctx = {false, nevr_quest::Source::kFile};
    cfg.voipdrop = {false, nevr_quest::Source::kFile};
    cfg.droneguard = {false, nevr_quest::Source::kFile};
    cfg.*c.member = {true, nevr_quest::Source::kFile};
    combat::SetConfig(cfg);
    combat::InstallNormal();
    const std::vector<Line> lines = Lines("combat_install");
    QCHECK(lines.size() == 1U);
    if (lines.size() == 1U) {
      if (!c.reader) QCHECK(Has(ResultsOf(lines[0]), "scope=off_config"));
      if (c.reader) QCHECK(!Has(ResultsOf(lines[0]), "scope=off_config"));
    }
    Done();
  }
}

// levelname and the booster pool write their stats at the end of the process (#2181), as the guard rows do: each row has an end step.
void TheLevelNameRowsHaveAnEndStep() {
  std::size_t count = 0;
  const combat::Module* table = combat::ShippedModules(&count);
  for (const char* name : {"levelname", "boosterpool"}) {
    const combat::Module* row = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
      if (std::strcmp(table[i].name, name) == 0) row = &table[i];
    }
    QCHECK(row != nullptr && row->early == nullptr && row->normal != nullptr && row->end != nullptr);
  }
}

// The end of the process asks every row's end step; with no guard installed (no libr15 on the host) none writes a stats line.
void TheEndOfTheProcessWritesNoStatsForAGuardThatIsNotInstalled() {
  Process(bv::Variant::kCombat);
  combat::InstallNormal();
  g_lines.clear();
  combat::NoteProcessEnd();
  for (const char* event : {"combat_fxtrail_stats", "combat_voipdrop_stats", "combat_custctx_stats", "combat_droneguard_stats", "combat_peerclamp_stats",
                            "combat_levelname_stats", "combat_boosterpool_stats"}) {
    QCHECK(Lines(event).empty());
  }
  Done();
}

// Each guard's switch is one `combat_config_switch` record, generated from the table: module, key, value and source (droneguard's second stage is a row of
// its own), and no other record carries them.
void TheGuardKeysAreLoggedAsOneSwitchRecordEachWithTheirSources() {
  Process(bv::Variant::kCombat);
  nevr_quest::CombatConfig cfg;
  cfg.fxtrail = {false, nevr_quest::Source::kFile};
  cfg.voipdrop = {true, nevr_quest::Source::kFile};  // each key a different value or source than its neighbours, so a row naming a neighbour's member is seen
  cfg.custctx = {false, nevr_quest::Source::kAbsent};
  cfg.droneguard = {true, nevr_quest::Source::kAbsent};
  cfg.droneguardStage1 = {true, nevr_quest::Source::kFile};
  cfg.peerclamp = {false, nevr_quest::Source::kFile};
  combat::LogConfig(cfg);
  combat::LogConfig(nevr_quest::CombatConfig{});
  struct Want {
    const char* module;
    const char* key;
    int on;
    const char* source;
  };
  const Want first[] = {{"fxtrail", "combat.fxtrail", 0, "config"},         {"voipdrop", "combat.voipdrop", 1, "config"},
                        {"custctx", "combat.custctx", 0, "default"},        {"droneguard", "combat.droneguard", 1, "default"},
                        {"droneguard_stage1", "combat.droneguard_stage1", 1, "config"}, {"peerclamp", "combat.peerclamp", 0, "config"}};
  const Want second[] = {{"fxtrail", "combat.fxtrail", 1, "default"},         {"voipdrop", "combat.voipdrop", 1, "default"},
                         {"custctx", "combat.custctx", 1, "default"},         {"droneguard", "combat.droneguard", 1, "default"},
                         {"droneguard_stage1", "combat.droneguard_stage1", 0, "default"}, {"peerclamp", "combat.peerclamp", 1, "default"}};
  const auto recordFor = [](const std::vector<Line>& lines, std::size_t pass, const char* key) -> const Line* {
    std::size_t seen = 0;
    for (const Line& l : lines) {
      if (!Has(l.text, std::string("\"key\":\"") + key + "\"")) continue;
      if (seen++ == pass) return &l;
    }
    return nullptr;
  };
  const std::vector<Line> lines = Lines("combat_config_switch");
  for (std::size_t pass = 0; pass < 2; ++pass) {
    for (const Want& w : pass == 0 ? first : second) {
      const Line* l = recordFor(lines, pass, w.key);
      QCHECK(l != nullptr);
      if (l == nullptr) continue;
      QCHECK(l->level == sentinel::LogLevel::kInfo);
      QCHECK(Has(l->text, std::string("\"event\":\"combat_config_switch\",\"module\":\"") + w.module + "\",\"key\":\"" + w.key + "\",\"on\":" +
                              std::to_string(w.on) + ",\"source\":\"" + w.source + "\"}"));
    }
  }
  QCHECK(Lines("combat_config_guards").empty());  // no record of its own beside the generated ones
  Done();
}

int main() {
  NamesAreStableTokens();
  EachPhaseRunsItsModulesInTableOrder();
  EachPhaseWritesOneAggregateLine();
  AModuleSwitchedOffByItsKeyIsCountedApart();
  TheEndOfTheProcessRunsEachModulesEndStep();
  AFailingModuleIsNamedAndTheOthersStillInstall();
  APlainReleaseInstallsNothingAndSaysSoOnce();
  NothingInstallsBeforeTheAnswerIsMade();
  APhaseRunsOnce();
  ALongTableWritesEveryModuleRecordAndAWholePhaseLine();
  TheShippedTableRowsAreWiredToTheirSteps();
  TheLevelNameRowsHaveAnEndStep();
  TheShippedTableHoldsHeapsThenMemsysEarlyAndTheTickTheRefreshRateAndTheTableStepsNormal();
  AShippedRowSwitchedOffByTheConfigIsCountedAsOff();
  TheRealStepsConsultTheGate();
  TheLevelNameRowsReadTheirSwitchAndTheBoosterNeedsLevelname();
  ANormalStepThatNeverRanIsRecordedAsNotStartedOnce();
  TheConfigLineNamesEachKeyItsValueAndItsSource();
  TheRefreshRateRowReadsItsThreeKeysFromTheResolvedSection();
  TheHudmeterAndShieldgammaRowsReadTheirKeysFromTheResolvedSection();
  TheGatesKeysHaveALineOfTheirOwn();
  TheGatesRowReadsItsKeysFromTheResolvedSection();
  ThePfxRowsReadTheirKeysFromTheResolvedSection();
  TheThreeSwitchesAreReportedOneRecordEachFromTheTable();
  TheGunAndBotRowsReadTheirKeysFromTheResolvedSection();
  TheBotsRowTellsTheModuleWhetherThisIsADevelopmentBuild();
  TheEcRowsReadTheirKeysFromTheResolvedSection();
  TheEcAndDroneModulesSwitchedOffByTheirKeyAreCountedOff();
  TheMusicAndSensorwdSwitchesAreOnByDefaultAndTheRowsReadThem();
  TheImpactsndAndDecalsRowsReadTheirKeysFromTheResolvedSection();
  TheBvhRowsReadTheirKeysFromTheResolvedSection();
  TheGuardRowsAreNormalPhaseRowsInThePluginsOrderEachWithAnEndStep();
  EachGuardRowReadsItsOwnSwitch();
  TheScopeRowStaysOnForEachGuardThatReadsItAndOffForTheOthers();
  TheEndOfTheProcessWritesNoStatsForAGuardThatIsNotInstalled();
  TheGuardKeysAreLoggedAsOneSwitchRecordEachWithTheirSources();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("combat_install_test: all checks passed");
  return 0;
}
