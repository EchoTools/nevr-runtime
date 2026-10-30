// The `ecfx` module (docs/quest/q15t/modules/ec.md): the client half of EC 1.1.7's NEUTRON effects deferral. Port of the source
// build's ecfx. The queue-mode wall lib (game content generated on the device: libac4e9374a146d3a4.so)
// queues {tp, entry | kind} on the EC page at +0xda0 instead of running a NEUTRON effect inside its own node, but only while the
// u32 at +0xe20 is non-zero. This module is the only writer of +0xe20: it drains the queue on the `ec` module's net tick and
// opens the gate (fail closed) only after the entries are resolved, the tick is registered and one drain has run.
// Off unless <data dir>/neutronfx.on exists, or the build is default-on (Q15T_ECFX_DEFAULT_ON: no file needed). No GOT word of its own: it rides ec's recvfrom wrapper.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

enum class EcfxStatus {
  kOff,         // no switch
  kWaiting,     // the wall lib is not loaded yet (gate stays 0)
  kOn,          // entries resolved, net tick registered, gate opens at the first drain
  kNoEntry,     // the lib is loaded but an entry is missing / not the named one: off (gate stays 0)
  kNoTickSlot,  // ec has no free net-tick slot: off (gate stays 0)
};

// Reads the switch; starts the thread that waits (once a second, quietly) for the wall lib. `base` is unused (no word is
// swapped here) and kept for the module signature. status kOk + reason "waiting" when the thread runs; reason "off" when off.
InstallResult InstallEcfx(std::uintptr_t base);

// One attach attempt (what the thread repeats): resolve the lib's four entries and register the net tick.
EcfxStatus EcfxAttach();

// The name the wall lib must have (game content; unverified here, see docs/quest/q15t/modules/ec.md (c)).
extern const char kEcfxLib[];

// test seams
void EcfxTickForTest(std::uint64_t now_ms);
void EcfxResetForTest();
bool EcfxGateOpenForTest();
void SetEcfxNoThreadForTest(bool on);
void EcfxSetEntryForTest(int kind, std::uint64_t fn);  // 0 charge, 1 burst, 2 boom, 3 sound: replaces the resolved entry

}  // namespace q15t
