// The hook on libr15's dlopen import (JUMP_SLOT 0x36c6380 in the pinned build): the seam the
// sentinel uses to run the installs that need a library the game loads after the constructor
// (post_load.h). The handler calls the real dlopen and, for a non-null handle, AfterDlopen.
//
// This file's translation unit is built -fno-exceptions (it includes callback_thunk.h): the handler
// is noexcept and has no landing pad. The one function it calls, AfterDlopen, carries a personality
// and is therefore marked NEVR_OUTSIDE_GAME_CALL (outside_game_call.h): it runs after the game's call
// has returned and calls no game code.
//
// Frequency: dlopen is called a handful of times per run (module loads), never per frame.
#pragma once

#include <atomic>
#include <cstdint>

#include "got_hook.h"

namespace nevr_quest::integration {

// libr15.so's dlopen slot, pinned to the artifact's build id and link-time address.
sentinel::GotTarget LibR15Dlopen();

enum class DlopenInstall : std::uint8_t { kOk, kFailed };

// Installs the thunk. `status` receives GotHook's answer (logged by GotHook itself as well).
DlopenInstall InstallDlopenHook(sentinel::GotStatus* status) noexcept;

// Registers the hook's counter (calls) with the reporter. Before StartReporter.
bool RegisterDlopenCounters() noexcept;

// Test seams: the handler's behaviour without a GOT slot. `original` stands for the real dlopen.
using DlopenFn = void* (*)(const char*, int);
void* RunDlopenHandlerForTest(DlopenFn original, const char* name, int flags) noexcept;

}  // namespace nevr_quest::integration
