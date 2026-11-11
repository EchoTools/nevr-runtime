#pragma once

// The combat module: he_is_the_cat's EchoCombat (heisthecat31/EchoCombat, nevr-pack c3ff4b54) built into
// nevr-runtime.dll for PC, its three plugins (dronefix.cpp, powerboost.cpp, veilnet.cpp) ported nearly as he wrote them.
// Which groups run is config.yaml's `combat` section (combat_settings.h has the table and the defaults). Every write the
// module makes to the game passes write_gate.h first: one that overlaps a range the runtime hooked or wrote is refused
// and its feature stays off. Logs under [NEVR.COMBAT] (docs/standards/logging.md).

#include <cstdint>
#include <string>
#include <vector>

namespace nevr_combat {

/// Reads the settings and starts the groups they turn on. Call once, after the runtime's own hooks are installed and
/// config.yaml can be read (boot.cpp, before the plugin pass). One Info line per group (enabled, off, or refused with
/// the reason), one per group that is never on, and a summary.
void Install(std::uintptr_t gameBase, bool isServer);

/// Registers the balance values' transform of the stock mp_weapon_settings (balance_merge.h). Call where the resource
/// transforms register (after InstallResourceOverride). It reads combat.balance at the first load it is asked about.
void RegisterBalance();

/// The features this process states to the clients that join it (runtime/compat/server_features.h, #2333): EchoCombat
/// when VeilNet started, and MEDIGUN when combat.medigun is on and it started. Empty before Install and when nothing
/// started. A server puts them in its ServerDB registration and in the lobby settings document it gives the game.
std::vector<std::string> ServerFeatures();

}  // namespace nevr_combat
