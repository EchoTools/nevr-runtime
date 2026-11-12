#include "quest/combat/pfx.h"

#include <atomic>

#include "quest/combat/combat_level.h"
#include "quest/combat/legacy_switch.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::pfx {

namespace {

// Stored once by Install; false until then, so the gate reads off before the install step has decided. An atomic with constant initialisation:
// no constructor runs at load time.
std::atomic<bool> g_on{false};

}  // namespace

GateDecision Decide(bool configOn) noexcept {
  if (!configOn) return {false, GateReason::kConfigOff};
  return {true, GateReason::kDefault};
}

ModuleResult Install(bool configOn) noexcept {
  const GateDecision decision = Decide(configOn);
  g_on.store(decision.on, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_pfx_install",
                      {{"state", decision.on ? "on" : "off"},
                       {"reason", decision.reason == GateReason::kConfigOff ? "off_config" : "default"},
                       {"switch", "combat.pfx"},
                       {"scope", "combat_map"},
                       {"msg", decision.on ? "the particle-renderer gate is on in combat maps only; it installs no hook and the renderer is not ported, so nothing "
                                             "changes in the game yet"
                                           : "combat.pfx is false: the particle-renderer gate stays shut"}});
  if (legacy::Present("pfx.off")) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_pfx_legacy_switch",
                        {{"file", "pfx.off"},
                         {"replaced_by", "combat.pfx: false"},
                         {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
  }
  return {decision.on, decision.on ? "installed" : "off_config", !decision.on};  // switched off by its key: counted apart (#2308)
}

bool GateOn() noexcept { return g_on.load(std::memory_order_acquire); }

bool InCombatMap(std::uintptr_t game) noexcept {
  constexpr bool kWithCombatLobby = false;  // the combat lobby is neither combat map nor arena: the gate is shut there
  std::uint64_t levelId = 0;
  return level::Classify(game, kWithCombatLobby, &levelId) == level::Kind::kCombatOrLobby;
}

bool GateOpenIn(std::uintptr_t game) noexcept { return GateOn() && InCombatMap(game); }

void ResetForTest() noexcept {
  g_on.store(false, std::memory_order_release);
}

void SetLegacyDirForTest(const char* dir) noexcept { legacy::SetDirectoryForTest(dir); }

}  // namespace nevr_quest::combat::pfx
