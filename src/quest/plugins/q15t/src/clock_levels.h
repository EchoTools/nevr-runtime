// The clock-level witness and candidate: a GOT wrapper on the game's import of vrapi_SetClockLevels.
//
// Always: log what the game asks for (levels, count, the runtime's answer). With SetClockRaise(true) (config.yaml `args: {clock: raise}`,
// read in NvrPluginInitEx; default off): ask the runtime for at least Q15T_CPU_LEVEL / Q15T_GPU_LEVEL instead of what the game asked
// for. The wrapper only ever raises a level. The switch is a runtime flag, so one binary serves both arms of the A/B.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

void SetClockRaise(bool raise);  // set before InstallClockLevels
bool ClockRaise();

InstallResult InstallClockLevels(std::uintptr_t base, std::uintptr_t slot_vaddr);

struct ClockStats {
  std::uint32_t calls;
  std::int32_t last_asked_cpu, last_asked_gpu, last_sent_cpu, last_sent_gpu;
};
ClockStats GetClockStats();

}  // namespace q15t
