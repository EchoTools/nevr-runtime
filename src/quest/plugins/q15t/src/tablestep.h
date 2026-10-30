// tablestep (issue #1552): the shader-set resource table's growth step. CFactory<CGShaderSetResource>::CreateInstance (ReVault
// libr15 0x1a0707c) gives the CTableXT it builds a growth step of 32, so a small table rounds up to 32 entries instead of
// doubling. The module sets the step of each object the stock function returns from 32 to 1 (the value the shipped variant's
// word row would have written). See tablestep.cpp for the reconstruction and the three slots that reach the function.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

// Installs the wrapper on each of the three slots that hold CreateInstance's address. A pin mismatch refuses the module
// (nothing swapped); a slot that does not hold the stock address is refused alone and logged, the others stay installed.
// `base` is the game library's load base. Logs one line per slot and one summary line.
InstallResult InstallTableStep(std::uintptr_t base);

struct TableStepStats {
  unsigned rewritten, unexpected;  // steps set from 32 to 1; objects whose step was not 32 (left alone)
};
TableStepStats GetTableStepStats();
void ResetTableStepForTest();

}  // namespace q15t
