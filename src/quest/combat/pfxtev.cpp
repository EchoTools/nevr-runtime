#include "quest/combat/pfxtev.h"

#include <atomic>
#include <cstdio>

#include "quest/combat/legacy_switch.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::pfxtev {

namespace {

constexpr std::uint32_t kLogCap = 64;  // event-id decision lines per process; the first line over it is one Warning, then silence

// Plain atomics with constant initialisation: no constructor runs at load.
std::atomic<bool> g_on{true};
std::atomic<std::uint32_t> g_lines{0}, g_allowed{0}, g_refused{0};

bool IsEventId(std::uint64_t id) noexcept {
  for (std::size_t i = 0; i < kIdCount; ++i) {
    if (kIds[i] == id) return true;
  }
  return false;
}

bool Decide(std::uint64_t id, bool inCombatMap) noexcept {
  if (!IsEventId(id)) return true;
  const bool switchOn = g_on.load(std::memory_order_relaxed);
  const bool allowed = switchOn && inCombatMap;
  (allowed ? g_allowed : g_refused).fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t n = g_lines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == kLogCap + 1) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_pfxtev_log_cap",
                        {{"cap", static_cast<long long>(kLogCap)}, {"msg", "further event-id decisions are counted but not written"}});
  }
  if (n > kLogCap) return allowed;
  char hex[24];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(id));
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_pfxtev_decision",
                      {{"id", hex},
                       {"result", allowed ? "allowed" : "refused"},
                       {"reason", !switchOn ? "switch" : (inCombatMap ? "switch" : "level")},
                       {"msg", allowed ? "the renderer may create this event effect"
                                       : "the renderer may not create this event effect; the game keeps its own behaviour"}});
  return allowed;
}

}  // namespace

ModuleResult Install(bool configOn) noexcept {
  g_on.store(configOn, std::memory_order_relaxed);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_pfxtev_install",
                      {{"state", configOn ? "on" : "off"},
                       {"reason", configOn ? "default" : "off_config"},
                       {"switch", "combat.pfxtev"},
                       {"msg", configOn ? "event-effect ids may be created in combat maps; nothing asks until the particle renderer lands"
                                        : "combat.pfxtev is false: event-effect ids are refused"}});
  if (legacy::Present("pfxtev.off")) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_pfxtev_legacy_switch",
                        {{"file", "pfxtev.off"},
                         {"replaced_by", "combat.pfxtev: false"},
                         {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
  }
  return {configOn, configOn ? "installed" : "off_config", !configOn};  // switched off by its key: counted apart (#2308)
}

bool AllowsIn(std::uint64_t id, bool inCombatMap) noexcept { return Decide(id, inCombatMap); }

Stats CurrentStats() noexcept { return Stats{g_allowed.load(std::memory_order_relaxed), g_refused.load(std::memory_order_relaxed)}; }

void ResetForTest() noexcept {
  g_on.store(true, std::memory_order_relaxed);
  g_lines.store(0, std::memory_order_relaxed);
  g_allowed.store(0, std::memory_order_relaxed);
  g_refused.store(0, std::memory_order_relaxed);
}

void SetLegacyDirForTest(const char* dir) noexcept { legacy::SetDirectoryForTest(dir); }

}  // namespace nevr_quest::combat::pfxtev
