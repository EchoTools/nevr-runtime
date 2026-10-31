// sendguard: NRadEngine::CSendEventNode::Enter (disassembly libr15 0x2825a80) sends a script event to the game space its node names,
// and does not check that the game space exists. When the named level is not loaded (a Quest without the combat content pack, once
// the menu gate lets the menu take the non-Quest branch) the lookup returns null and the stock function reads [null + 0x58] and
// faults. The module wraps Enter and drops the send, with one log line, when the lookup comes back null; every other send runs the
// stock function unchanged. See sendguard.cpp for the reconstruction and the slots that reach the function.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

// Installs the wrapper on each data slot that holds Enter's address. A pin mismatch refuses the module (nothing swapped); a slot
// that does not hold the stock address is refused alone and logged, the others stay installed. `base` is the game library's load
// base. Logs one line per slot and one summary line.
InstallResult InstallSendGuard(std::uintptr_t base);

struct SendGuardStats {
  unsigned dropped, passed;  // sends dropped because the named game space is not loaded; sends that reached the stock function
};
SendGuardStats GetSendGuardStats();
void ResetSendGuardForTest();

}  // namespace q15t
