#include "runtime/combat/combat.h"

#include <winsock2.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/logging.h"
#include "runtime/combat/balance_merge.h"
#include "runtime/combat/balance_values.h"
#include "runtime/combat/capture_scores.h"
#include "runtime/combat/capture_time.h"
#include "runtime/combat/cat_compat.h"
#include "runtime/combat/cat_links.h"
#include "runtime/combat/clock_pause.h"
#include "runtime/combat/combat_settings.h"
#include "runtime/combat/dot_script.h"
#include "runtime/combat/equip_cell.h"
#include "runtime/combat/orb_pause.h"
#include "runtime/combat/point_rate.h"
#include "runtime/combat/rules_button.h"
#include "runtime/combat/rules_state.h"
#include "runtime/combat/script_guard.h"
#include "runtime/combat/shared_magazine.h"
#include "runtime/combat/station_cells.h"
#include "runtime/combat/team_ready.h"
#include "runtime/combat/write_gate.h"
#include "runtime/compat/server_features.h"
#include "runtime/lifecycle/game_image_guard.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/resource_override.h"

namespace nevr_combat {
namespace {

// The cells Cat's scripts and his three plugins share: the zero tail of echovr.exe's last .data page, past its declared
// size (VirtualSize 0x108ca4 from VA 0x141ffc000), mapped read/write and never used by the game. Tokens, counters, the
// FX queue, the drone death token, the equip guard, the RULES mailbox: RVA 0x2104ca4 up to the page end 0x2105000.
constexpr std::uintptr_t kCellsRva = 0x2104ca4;
constexpr std::size_t kCellsLength = 0x2105000 - kCellsRva;

const char* OnOff(bool on) { return on ? "on" : "off"; }

void LogGroup(const GroupSpec& spec, const Setting& wanted, bool started, const char* reason) {
  if (!wanted.on) {
    Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] feature off name=\"%s\" key=combat.%s source=%s", spec.name, spec.key,
        SourceName(wanted.source));
  } else if (started) {
    Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] feature enabled name=\"%s\" key=combat.%s source=%s", spec.name, spec.key,
        SourceName(wanted.source));
  } else {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] feature refused name=\"%s\" key=combat.%s source=%s reason=%s (the feature stays off)", spec.name,
        spec.key, SourceName(wanted.source), reason);
  }
}

// --- balance -----------------------------------------------------------------------------------------------------

std::mutex g_featuresLock;
std::vector<std::string> g_serverFeatures;  // guarded by g_featuresLock; set once by Install

std::mutex g_balanceLock;
std::deque<std::string> g_balanceBuffers;  // every buffer served stays valid for the process (the game keeps reading it)
int g_balanceOn = -1;                      // -1: not read yet

bool BalanceTransform(const void* original, uint64_t originalSize, const void** out, uint64_t* outSize, void*) {
  std::lock_guard<std::mutex> lock(g_balanceLock);
  if (g_balanceOn < 0) {
    const Settings s = ReadSettings([](const char* key) { return NevrCfgCombatValue(key); });
    const Setting& b = s.groups[static_cast<std::size_t>(Group::kBalance)];
    g_balanceOn = b.on ? 1 : 0;
    LogGroup(kGroups[static_cast<std::size_t>(Group::kBalance)], b, b.on, "");
  }
  if (g_balanceOn == 0) return false;
  std::string merged;
  std::size_t changed = 0;
  const balance::Result r = balance::Apply(static_cast<const std::uint8_t*>(original), static_cast<std::size_t>(originalSize),
                                           balance::kCatWeaponSettings, &merged, &changed);
  if (r != balance::Result::kApplied) {
    Log(r == balance::Result::kUnchanged ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] balance not applied resource=mp_weapon_settings result=%s size=%llu (the game's own values stay)",
        balance::ResultName(r), static_cast<unsigned long long>(originalSize));
    return false;
  }
  g_balanceBuffers.push_back(std::move(merged));
  *out = g_balanceBuffers.back().data();
  *outSize = g_balanceBuffers.back().size();
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] balance applied resource=mp_weapon_settings values=%llu size=%llu->%llu (Cat's weapon tuning merged into "
      "the stock settings)",
      static_cast<unsigned long long>(changed), static_cast<unsigned long long>(originalSize),
      static_cast<unsigned long long>(*outSize));
  return true;
}

}  // namespace

void RegisterBalance() {
  const bool ok = RegisterResourceTransform(balance::kJsonResourceType, balance::kWeaponSettingsName, &BalanceTransform,
                                            nullptr, "EchoCombat balance values");
  Log(ok ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.COMBAT] balance transform %s type=0x%016llx name=0x%016llx", ok ? "registered" : "not registered (no registry)",
      static_cast<unsigned long long>(balance::kJsonResourceType),
      static_cast<unsigned long long>(balance::kWeaponSettingsName));
}

void Install(std::uintptr_t gameBase, bool isServer) {
  static std::once_flag once;
  std::call_once(once, [gameBase, isServer] {
    const Settings s = ReadSettings([](const char* key) { return NevrCfgCombatValue(key); });
    const auto at = [&](Group g) -> const Setting& { return s.groups[static_cast<std::size_t>(g)]; };
    const auto spec = [](Group g) -> const GroupSpec& { return kGroups[static_cast<std::size_t>(g)]; };

    for (const Disabled& d : kNeverOn) {
      Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] feature disabled name=\"%s\" reason=\"%s\" (never on, whatever the config says)",
          d.name, d.reason);
    }

    // Every address Cat checks and writes is one build's: the runtime's image guard says this is that build (echovr.exe
    // 631547, sha256 b6d08277...2043; game_image_guard.cpp). Each site's own bytes are checked again before its write.
    if (!nevr_game_image_guard::IsSupportedGameModule(reinterpret_cast<const void*>(gameBase))) {
      for (const GroupSpec& g : kGroups) {
        if (g.group != Group::kBalance) LogGroup(g, at(g.group), false, "unsupported_game_build");
      }
      Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] combat module not started reason=unsupported_game_build");
      return;
    }

    cat::InstallFaultHandler();

    // A stock-script fix, like the drone follow guard: the DOT FIELD's own script plays sounds that exist and stops
    // looking for a model it does not have (dot_script.h). It changes no gameplay value, so no group gates it.
    dot_script::Register();

    // A stock-script fix: a thrown DOT FIELD or drone no longer asks for a frisbee component it does not have
    // (shared_magazine.h). It changes no gameplay value, so no group gates it.
    shared_magazine::Register();

    // The shared cells are written by Cat's scripts and code alike: checked once against everything the runtime owns.
    const bool cells = GateWrite(reinterpret_cast<const void*>(gameBase + kCellsRva), kCellsLength,
                                 "EchoCombat .data-tail cells (tokens, counters, FX queue, RULES mailbox)");
    const char* const cellsRefused = "data_tail_cells_overlap";

    // The drone follow crash guard is a stock-game fix: it runs whatever combat.drone says.
    const bool drone = at(Group::kDrone).on && cells;
    const bool guard = dronefix::Start(drone);
    Log(guard ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
        "[NEVR.COMBAT] feature %s name=\"drone follow crash guard\" group=stock-fixes va=0x%llx%s", guard ? "enabled" : "refused",
        static_cast<unsigned long long>(gameBase + 0xd65cb0),
        guard ? " (Follow skips while its target is null)" : " reason=hook_not_installed (see the lines above)");
    LogGroup(spec(Group::kDrone), at(Group::kDrone), guard && drone,
             !cells ? cellsRefused : "follow_update_hook_not_installed");

    const bool veilWanted = at(Group::kVeil).on && cells;
    const bool match = at(Group::kMatchControl).on && veilWanted;
    const bool veil = veilWanted && veilnet::Start(isServer, match, at(Group::kMedigun).on);
    // The script guards read the tac mod VeilNet records from the player's profile update (equip_cell.h): with VeilNet off
    // nothing records it, so no guard would ever act.
    if (veil) script_guard::Register(reinterpret_cast<volatile std::uint64_t*>(gameBase + kEquipCellRva), isServer);
    // The equipment station's grids follow the cell counts VeilNet publishes for LOADOUTS (station_cells.h); with VeilNet
    // off none is published and the station keeps its stock layout.
    if (veil) {
      station_cells::Register(reinterpret_cast<const volatile std::uint8_t*>(gameBase + station_cells::kRulesMailboxRva +
                                                                             station_cells::kCellCountsOffset));
    }
    LogGroup(spec(Group::kVeil), at(Group::kVeil), veil, !cells ? cellsRefused : "sendto_recvfrom_hooks_not_installed");
    LogGroup(spec(Group::kMatchControl), at(Group::kMatchControl), veilnet::MatchControlStarted(),
             !at(Group::kVeil).on ? "needs_combat.veil"
             : !cells             ? cellsRefused
             : !veil              ? "combat_net_not_started"
             : !veilnet::EventDispatchHooked() ? "event_dispatch_not_hooked"
                                               : "slot_setter_filter_not_installed");

    // PAUSE gives the players the pre-match spawn orbs, and the orbs say when the match is live (orb_pause.h): both go
    // through the RULES mailbox VeilNet's match control keeps, so they register with it.
    // PAUSE also holds Combat's clocks (clock_pause.h), read from the same mailbox.
    // POINT RATE and PAUSE also set how fast a capture point's points rise (point_rate.h), from the same mailbox.
    // The RULES page's starting scores reach the capture points through the same mailbox (capture_scores.h).
    if (veilnet::MatchControlStarted()) {
      orb_pause::Register(reinterpret_cast<volatile std::uint8_t*>(gameBase + rules::kMailboxRva));
      clock_pause::Register(reinterpret_cast<volatile std::uint8_t*>(gameBase + rules::kMailboxRva));
      point_rate::Register(reinterpret_cast<volatile std::uint8_t*>(gameBase + rules::kMailboxRva));
      capture_scores::Register(reinterpret_cast<volatile std::uint8_t*>(gameBase + rules::kMailboxRva));
      capture_time::Register(reinterpret_cast<volatile std::uint8_t*>(gameBase + rules::kMailboxRva));
    }

    // TEAM READY (a podium terminal button) works with more than one player and for demo accounts: hooks on Combat's
    // game-mode script, installed when it loads (team_ready.h). It needs nothing of VeilNet's.
    if (at(Group::kMatchControl).on) team_ready::Register();
    // The podium's RULES button is pressable in Combat (rules_button.h); it too needs nothing of VeilNet's.
    if (at(Group::kMatchControl).on) rules_button::Register();

    const bool overcharge = at(Group::kOvercharge).on && cells && powerboost::Start(isServer, at(Group::kMedigun).on);
    // logging.md Rule 11: the settings PowerBoost (and MEDIGUN, when on) just read, by where each came from; the per-key
    // source lines are Debug (cat_compat.cpp).
    if (overcharge) {
      const cat::IniSourceCounts sources = cat::IniSources();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.COMBAT] PowerBoost settings read config_yaml=%d shipped=%d code=%d (a key config.yaml leaves out takes the "
          "value his release ships; code is a key his PowerBoost.ini does not carry)",
          sources.config, sources.shipped, sources.code);
    }
    LogGroup(spec(Group::kOvercharge), at(Group::kOvercharge), overcharge,
             !cells ? cellsRefused : "heal_damage_hooks_not_installed");
    LogGroup(spec(Group::kMedigun), at(Group::kMedigun), overcharge && veil,
             !at(Group::kOvercharge).on ? "needs_combat.overcharge" : !veil ? "needs_combat.veil" : "overcharge_not_started");

    // On by default, as his loader applies it on every start; the TerminateProcess stub is never written while combat.clean_exit is off.
    const bool cleanExit = at(Group::kCleanExit).on && veilnet::StartCleanExit();
    LogGroup(spec(Group::kCleanExit), at(Group::kCleanExit), cleanExit, "teardown_call_not_patched (see the line above)");

    int enabled = guard ? 1 : 0;
    enabled += (guard && drone) + veil + veilnet::MatchControlStarted() + overcharge + (overcharge && veil && at(Group::kMedigun).on) +
               cleanExit;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.COMBAT] combat module started server=%s features_enabled=%d drone=%s overcharge=%s medigun=%s veil=%s "
        "match_control=%s clean_exit=%s (balance is reported when mp_weapon_settings loads)",
        isServer ? "true" : "false", enabled, OnOff(guard && drone), OnOff(overcharge),
        OnOff(overcharge && veil && at(Group::kMedigun).on), OnOff(veil), OnOff(veilnet::MatchControlStarted()),
        OnOff(cleanExit));

    // #2333: the features a server states to its clients, from what actually started (server_features.h).
    std::vector<std::string> features =
        nevr_server_features::CombatFeatures(veil, at(Group::kMedigun).on, overcharge);
    if (isServer) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.COMBAT] server features features=%s (the registration and the lobby settings carry them, so a client "
          "shows the EchoCombat tac mods%s only on a server that states them)",
          nevr_server_features::Joined(features).c_str(), features.size() > 1 ? " and MEDIGUN" : "");
    }
    std::lock_guard<std::mutex> lock(g_featuresLock);
    g_serverFeatures = std::move(features);
  });
}

std::vector<std::string> ServerFeatures() {
  std::lock_guard<std::mutex> lock(g_featuresLock);
  return g_serverFeatures;
}

}  // namespace nevr_combat
