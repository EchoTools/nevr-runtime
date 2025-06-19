

#include "globals.h"

#include "echovrInternal.h"
/// A CLI argument flag used to remove the extra console being added by -headless for running servers on fully headless
/// system.
// Initialize external variables from opts.h
BOOL noConsole = FALSE;
/// A CLI argument flag indicating whether the game is booting in headless mode (no graphics/audio).
BOOL isHeadless = FALSE;

/// A timestep value in ticks/updates per second, to be used for headless mode (due to lack of GPU/refresh rate
/// throttling). If non-zero, sets the timestep override by the given tick rate per second. If zero, removes tick rate
/// throttling.
UINT32 headlessTickRateHz = 120;