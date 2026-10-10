// The CJson::TString thunk translation unit's interface.
//
// callback_thunk.h, hook_install.h and pinned_targets.h may be included only by translation units
// built with -fno-exceptions (docs/adr/0003, "Hook frames carry no personality"): the game uses
// libgcc's unwinder through libc++_shared.so and the sentinel statically links LLVM libunwind, so a
// frame that names the sentinel's personality must never sit between a game call and the game's
// handler. Everything that needs exceptions (the policy, the pool, std::string, the cache lock)
// therefore lives in other translation units, and this header is the whole boundary between them:
// no thunk type, no exception, nothing that needs a landing pad.
#pragma once

#include <cstdint>

#include "got_hook.h"

namespace nevr_quest::redirect {

enum class Slot : std::uint8_t { kLibR15, kMatchmaking };

struct HookTargets {
  sentinel::GotTarget libr15;
  sentinel::GotTarget matchmaking;
};
// The pinned production targets (build ID and slot address pinned).
HookTargets PinnedTargets();

// The decision function the handler calls after the game's original has returned: it receives the
// key and the value the original returned and answers with `result` itself or a stable pointer.
// noexcept, and it must not call game code. It is reached through a function pointer, not a direct
// call, so the thunk translation unit has no call edge into the exceptions-enabled code: the
// callee's frames are sentinel-only and are never on the stack across a call into the game.
using ApplyFn = const char* (*)(const char* key, const char* result) noexcept;
void SetApply(ApplyFn apply) noexcept;  // nullptr: handlers pass the original result through

// The thunk for one slot. All noexcept; defined in tstring_thunks.cpp (built -fno-exceptions).
sentinel::GotStatus InstallThunk(Slot slot, sentinel::GotHook& hook, const sentinel::GotTarget& target,
                                 sentinel::ImageLookup lookup) noexcept;
void* ThunkEntry(Slot slot) noexcept;         // the entry the GOT slot receives (tests call it directly)
void** ThunkOriginalOut(Slot slot) noexcept;  // test support: publish a fake original
void ArmThunk(Slot slot, bool armed) noexcept;
void ResetThunk(Slot slot) noexcept;          // test support: clears original, handler, counters

// Registers the four thunk counters (calls and faults for each slot) with the sentinel's reporter.
// Every registration must precede StartReporter; returns false if the table is full or the reporter
// already runs.
bool RegisterThunkCounters() noexcept;

}  // namespace nevr_quest::redirect
