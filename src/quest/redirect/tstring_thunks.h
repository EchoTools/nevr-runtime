// The CJson::TString thunk translation unit's interface.
//
// callback_thunk.h and pinned_targets.h may be included only by translation units built with
// -fno-exceptions (docs/adr/0003, "Hook frames carry no personality"): the game uses libgcc's
// unwinder through libc++_shared.so and the sentinel statically links LLVM libunwind, so a frame
// that names the sentinel's personality must never sit between a game call and the game's
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

// The thunk for one slot. All noexcept; defined in tstring_thunks.cpp (built -fno-exceptions).
void* ThunkEntry(Slot slot) noexcept;         // address to install into the GOT slot
void** ThunkOriginalOut(Slot slot) noexcept;  // where GotHook::Install publishes the original
void ArmThunk(Slot slot, bool armed) noexcept;
void ResetThunk(Slot slot) noexcept;          // test support: clears original, handler, counters

// Implemented in hook_adapter.cpp (exceptions enabled). Called by the handler with the key and the
// value the game's original returned, after the original has returned. Returns `result` itself, or a
// pointer into the stable string pool. noexcept: it catches std::exception and the specific
// library types inside its own frames, which never call the game, and returns `result` on any
// fault. The handler never calls the original from a frame that has a landing pad.
const char* ApplyActive(const char* key, const char* result) noexcept;

}  // namespace nevr_quest::redirect
