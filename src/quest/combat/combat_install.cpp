#include "quest/combat/combat_install.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_active.h"
#include "quest/combat/sendguard.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat {

namespace {

// The registration table, in install order (combat_install.h, "The table"). Each module's unit adds its row at the position the q15t
// plugin installed it, and nowhere else. The count is deduced.
constexpr std::array kModules{
    Module{"sendguard", nullptr, &sendguard::Install},
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

// The configuration the sequence resolved, kept for the modules' install functions. Written once by the constructor's configuration step,
// on the constructor's thread, before any install step reads it; a struct of two bools and two enums, constant-initialised.
namespace {
nevr_quest::CombatConfig g_config;
}  // namespace

const nevr_quest::CombatConfig& Config() noexcept { return g_config; }

void LogConfig(const nevr_quest::CombatConfig& config) noexcept {
  g_config = config;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_config",
                      {{"force", config.force.on ? 1 : 0}, {"force_source", config.force.source == nevr_quest::Source::kFile ? "config" : "default"},
                       {"sendguard", config.sendguard.on ? 1 : 0},
                       {"sendguard_source", config.sendguard.source == nevr_quest::Source::kFile ? "config" : "default"},
                       {"msg", "combat.force asks for the folded combat modules whatever the APK's variant; only a development build "
                               "honours it (see combat_active); a module's own key (sendguard) turns that module off"}});
}

void ResetInstallForTest() noexcept {
  g_config = nevr_quest::CombatConfig{};
  StateOf(Phase::kEarly).store(kNotYet, std::memory_order_release);
  StateOf(Phase::kNormal).store(kNotYet, std::memory_order_release);
  g_inactiveSaid.store(false, std::memory_order_release);
}

}  // namespace nevr_quest::combat
