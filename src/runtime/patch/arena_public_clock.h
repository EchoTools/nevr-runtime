#pragma once

// The match clock of a public Arena match keeps counting through goals on a server, like a private match (#982).
// The decisions, the evidence and the script sites are in arena_public_clock_rules.h.

#ifdef NEVR_ARENA_PUBLIC_CLOCK_TESTING
#include <cstddef>
#include <cstdint>
#endif

namespace nevr_arena_public_clock {

// Server only. Hooks the net-flag bind function of the two Arena scripts that store the public-match flag, in each
// script DLL as it loads (a post-load observer on the LoadLibrary hooks) and again whenever the game unloads and
// reloads it. A site is hooked only after its bytes are validated; a mismatch is logged at Warning and that script
// keeps the game's behaviour. Call after nevr_dll_load_hook::Install().
void Install();

#ifdef NEVR_ARENA_PUBLIC_CLOCK_TESTING
using BindFn = std::int64_t (*)(void* state, void* node);
/// One observer pass: hooks every mapped, unhooked site (test_arena_public_clock_hook.cpp).
void RefreshForTest();
/// The trampoline a site's detour calls after its decision; a test swaps in a recorder, since the script's own
/// bind function needs the engine's callbacks.
BindFn OriginalForTest(std::size_t site);
void SetOriginalForTest(std::size_t site, BindFn original);
#endif

}  // namespace nevr_arena_public_clock
