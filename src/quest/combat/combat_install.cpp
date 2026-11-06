#include "quest/combat/combat_install.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_active.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/heaps.h"
#include "quest/combat/hudmeter.h"
#include "quest/combat/memsys.h"
#include "quest/combat/refresh_rate.h"
#include "quest/combat/shieldgamma.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat {

namespace {

// The configuration the rows read their switch from (SetConfig). A plain object: it is written once on the constructor's thread before
// either step runs, and the normal step's thread is ordered after it by the post-load action's atomics.
nevr_quest::CombatConfig g_config;

// A row's install function reads its module's switch here, so a module's own code takes the switch as an argument and a host test
// drives it without a configuration.
ModuleResult HeapsEarly() noexcept { return heaps::Install(g_config.heaps.on, g_config.memsys.on); }
ModuleResult MemsysEarly() noexcept { return memsys::Install(g_config.memsys.on); }
ModuleResult RefreshRateNormal() noexcept {
  return refresh_rate::Install(g_config.refreshRate.on, g_config.refreshRateLobby.on, g_config.refreshRateQuest2At72.on);
}
ModuleResult HudmeterNormal() noexcept { return hudmeter::Install(g_config.hudmeter.on); }
ModuleResult ShieldgammaNormal() noexcept {
  return shieldgamma::Install(g_config.shieldgammaGamma.on, g_config.shieldgammaTubeMirror.on);
}

// The registration table, in install order (combat_install.h, "The table"). Each module's unit adds its row at the position the q15t
// plugin installed it, and nowhere else; the count is deduced.
//   heaps, then memsys: the heap sizes are ours before the pool and the witness hook go in (the plugin's order); both are early,
//   before android_main reads the sizes.
constexpr std::array kModules{
    Module{"heaps", &HeapsEarly, nullptr},
    Module{"memsys", &MemsysEarly, nullptr},
    // tick, then refresh_rate (#2172): the game-thread tick installs first (combat_tick.h), and every module with a per-frame callback registers
    // it from its own step, which may come before or after the tick's install.
    Module{"tick", nullptr, &tick::InstallNormal},
    // scope (#2186): the combat level answer (combat_scope.h) is refreshed from the tick, so it comes after the tick's install and before every
    // module that reads it.
    Module{"scope", nullptr, &scope::InstallNormal},
    Module{"refresh_rate", nullptr, &RefreshRateNormal},
    // hudmeter, then shieldgamma (#2186): both read the combat level answer.
    Module{"hudmeter", nullptr, &HudmeterNormal},
    Module{"shieldgamma", nullptr, &ShieldgammaNormal},
};

// Per phase: 0 not yet, 1 ran or was found inactive, 2 ended as not started. One atomic each: the early step runs on the constructor's
// thread, the normal step on the thread that returned from the game's first dlopen, and the process end on a helper thread.
std::atomic<int> g_phaseState[2] = {{0}, {0}};
constexpr int kNotYet = 0;
constexpr int kDone = 1;
constexpr int kEnded = 2;
// A process where combat does not run says so once, whichever phase asks first.
std::atomic<bool> g_inactiveSaid{false};

std::atomic<int>& StateOf(Phase phase) { return g_phaseState[phase == Phase::kEarly ? 0 : 1]; }

const char* const kInactiveMessage = "folded combat modules do not install in this run; the game keeps its own behaviour";

void WriteSkipped(Phase phase) {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_install",
                      {{"phase", PhaseName(phase)}, {"state", "skipped"}, {"variant", build_variant::VariantName(RecordedVariant())},
                       {"active", 0}, {"active_reason", ReasonName(ActiveReason())}, {"modules", 0}, {"installed", 0},
                       {"failed", 0}, {"msg", kInactiveMessage}});
}

}  // namespace

const char* PhaseName(Phase phase) noexcept { return phase == Phase::kEarly ? "early" : "normal"; }

Outcome RunPhase(Phase phase, const Module* table, std::size_t count) noexcept {
  Outcome outcome;
  int expected = kNotYet;
  if (!StateOf(phase).compare_exchange_strong(expected, kDone, std::memory_order_acq_rel)) return outcome;  // ran, or ended
  if (!Active()) {
    if (!g_inactiveSaid.exchange(true, std::memory_order_acq_rel)) WriteSkipped(phase);
    return outcome;
  }
  char results[320];
  results[0] = '\0';
  std::size_t used = 0;
  bool truncated = false;
  for (std::size_t i = 0; i < count; ++i) {
    const Module& module = table[i];
    const InstallFn fn = phase == Phase::kEarly ? module.early : module.normal;
    if (fn == nullptr) continue;
    const ModuleResult result = fn();
    ++outcome.modules;
    if (result.installed) {
      ++outcome.installed;
    } else {
      ++outcome.failed;
    }
    const int wrote = std::snprintf(results + used, sizeof(results) - used, "%s%s=%s", used == 0 ? "" : " ", module.name,
                                    result.status != nullptr ? result.status : "unset");
    if (wrote < 0 || static_cast<std::size_t>(wrote) >= sizeof(results) - used) {
      truncated = true;  // the counts are exact; the list is cut where it stopped fitting
      results[used] = '\0';
    } else {
      used += static_cast<std::size_t>(wrote);
    }
  }
  outcome.ran = true;
  char msg[160];
  if (outcome.modules == 0) {
    std::snprintf(msg, sizeof(msg), "no combat module has a step in this phase yet; nothing of combat's is installed");
  } else {
    std::snprintf(msg, sizeof(msg), "%zu of %zu combat modules installed in this phase%s", outcome.installed, outcome.modules,
                  outcome.failed == 0 ? "" : "; the others keep the game's own behaviour");
  }
  sentinel::LogFields(outcome.failed == 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "combat_install",
                      {{"phase", PhaseName(phase)}, {"state", "ran"}, {"variant", build_variant::VariantName(RecordedVariant())},
                       {"active", 1}, {"active_reason", ReasonName(ActiveReason())}, {"modules", outcome.modules},
                       {"installed", outcome.installed}, {"failed", outcome.failed}, {"results", results},
                       {"results_truncated", truncated ? 1 : 0}, {"msg", msg}});
  return outcome;
}

Outcome InstallEarly() noexcept { return RunPhase(Phase::kEarly, kModules.data(), kModules.size()); }

Outcome InstallNormal() noexcept { return RunPhase(Phase::kNormal, kModules.data(), kModules.size()); }

void NoteNormalNotStarted(const char* reasonToken) noexcept {
  if (!Active()) return;  // combat does not run here: nothing was owed
  int expected = kNotYet;
  if (!StateOf(Phase::kNormal).compare_exchange_strong(expected, kEnded, std::memory_order_acq_rel)) return;  // ran, or already ended
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_install",
                      {{"phase", "normal"}, {"state", "not_started"}, {"reason", reasonToken != nullptr ? reasonToken : "unset"},
                       {"variant", build_variant::VariantName(RecordedVariant())}, {"active", 1},
                       {"active_reason", ReasonName(ActiveReason())}, {"modules", 0}, {"installed", 0}, {"failed", 0},
                       {"msg", "the combat modules' post-load step had no moment to run in, so none of them is installed; "
                               "the game keeps its own behaviour"}});
}

void LogConfig(const nevr_quest::CombatConfig& config) noexcept {
  const auto source = [](const nevr_quest::Source s) { return s == nevr_quest::Source::kFile ? "config" : "default"; };
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_config",
                      {{"force", config.force.on ? 1 : 0}, {"force_source", config.force.source == nevr_quest::Source::kFile ? "config" : "default"},
                       {"heaps", config.heaps.on ? 1 : 0}, {"heaps_source", config.heaps.source == nevr_quest::Source::kFile ? "config" : "default"},
                       {"memsys", config.memsys.on ? 1 : 0}, {"memsys_source", config.memsys.source == nevr_quest::Source::kFile ? "config" : "default"},
                       {"refresh_rate", config.refreshRate.on ? 1 : 0}, {"refresh_rate_source", source(config.refreshRate.source)},
                       {"refresh_rate_lobby", config.refreshRateLobby.on ? 1 : 0}, {"refresh_rate_lobby_source", source(config.refreshRateLobby.source)},
                       {"refresh_rate_quest2_72", config.refreshRateQuest2At72.on ? 1 : 0},
                       {"refresh_rate_quest2_72_source", source(config.refreshRateQuest2At72.source)},
                       {"hudmeter", config.hudmeter.on ? 1 : 0}, {"hudmeter_source", source(config.hudmeter.source)},
                       {"shieldgamma_gamma", config.shieldgammaGamma.on ? 1 : 0}, {"shieldgamma_gamma_source", source(config.shieldgammaGamma.source)},
                       {"shieldgamma_tube_mirror", config.shieldgammaTubeMirror.on ? 1 : 0},
                       {"shieldgamma_tube_mirror_source", source(config.shieldgammaTubeMirror.source)},
                       {"msg", "combat.force asks for the folded combat modules whatever the APK's variant; only a development build "
                               "honours it (see combat_active); a module's own key set to false turns that module off"}});
}

void SetConfig(const nevr_quest::CombatConfig& config) noexcept { g_config = config; }

void ResetInstallForTest() noexcept {
  g_config = nevr_quest::CombatConfig{};
  StateOf(Phase::kEarly).store(kNotYet, std::memory_order_release);
  StateOf(Phase::kNormal).store(kNotYet, std::memory_order_release);
  g_inactiveSaid.store(false, std::memory_order_release);
}

}  // namespace nevr_quest::combat
