/* NEUTRON effects (#2187, docs/quest/combat/ec.md): the client half of EC 1.1.7's NEUTRON effects deferral. The queue-mode wall library (game
 * content generated on the device: libac4e9374a146d3a4.so) queues {tp, entry | kind} on re33_ec_page at +0xda0 instead of running a NEUTRON
 * effect inside its own node, but only while the u32 at +0xe20 is non-zero. This module is the only writer of +0xe20: it drains the queue on
 * the EC client's net tick (ec::OnNetTick) and opens the gate (fail closed) only after the four entries are resolved, the tick is registered
 * and one drain has run. No hook of its own: it rides ec's recvfrom handler, so it runs on a combat map or the combat lobby only (ec.h,
 * "Arena"); in any other level no net tick runs and the gate is never opened by a drain there.
 *
 * Config: `combat.ecfx` (default true). The plugin's `neutronfx.on` file is no longer read: one found is a Warning
 * (`combat_ecfx_legacy_switch`). Needs ec: with ec not installed the step refuses (`needs_ec`).
 *
 * The thread. Until the wall library is loaded (it loads with the first EC level), a thread looks for it once a second with
 * dlopen(RTLD_NOLOAD) and stops once it is found or refused.
 *
 * Origin (rule 5 of src/quest/combat/README.md; docs/audits/2026-11-05-q15t-origin-citations.md rows 54, 61, 210-211). The q15t plugin's
 * `ecfx.cpp` / `ecfx_core.h`, a port of the reference build's `runtime/native/afkxr/afk_ecfx.cpp` and `afk_ecfx_core.h` (at `1357409`), whose
 * rules are EchoCombat 1.1.7's NEUTRON effects deferral in DroneFix, `plugin/dronefix.cpp` at `c14c3e0a` L332-409 (WallFxDue L374-409, SoundAgain
 * L352-359), L682-689, L1683-1686.
 *
 * Lines (sentinel log): `combat_ecfx_install`, `combat_ecfx_waiting`, `combat_ecfx_attach`, `combat_ecfx_gate`, `combat_ecfx_ran`,
 * `combat_ecfx_drain`, `combat_ecfx_legacy_switch`. The ran and drain records: the first 96 in all, then `combat_ecfx_lines` (later ones
 * are counted), then `combat_ecfx_sum` with the counts at most every 10 s and once none has come for 10 s (#2349).
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"

namespace nevr_quest::combat::ecfx {

enum class Status {
  kWaiting,     // the wall lib is not loaded yet (gate stays 0)
  kOn,          // entries resolved, net tick registered, gate opens at the first drain
  kNoEntry,     // the lib is loaded but an entry is missing / not the named one: off (gate stays 0)
  kNoTickSlot,  // ec has no free net-tick slot: off (gate stays 0)
};

// The name the wall lib must have (game content; unverified here, docs/quest/combat/ec.md (c)).
extern const char kWallLib[];

// The step. Fixed tokens: "waiting" (the thread looks for the wall lib), "off", "needs_ec", "thread", "already_ran".
ModuleResult Install(bool on) noexcept;

// One attach attempt (what the thread repeats): resolve the lib's four entries and register the net tick.
Status Attach() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------
void TickForTest(std::uint64_t nowMs) noexcept;
void ResetForTest() noexcept;
bool GateOpenForTest() noexcept;
void SetNoThreadForTest(bool on) noexcept;
void SetEntryForTest(int kind, std::uint64_t fn) noexcept;  // 0 charge, 1 burst, 2 boom, 3 sound: replaces the resolved entry

}  // namespace nevr_quest::combat::ecfx
