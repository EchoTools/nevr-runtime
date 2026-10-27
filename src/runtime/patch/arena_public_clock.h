#pragma once

// The match clock of a public Arena match keeps counting through goals on a server, like a private match (#982).
// The decisions, the evidence and the script sites are in arena_public_clock_rules.h.

namespace nevr_arena_public_clock {

// Server only. Hooks the net-flag bind function of the two Arena scripts that store the public-match flag, in each
// script DLL as it loads (a post-load observer on the LoadLibrary hooks) and again whenever the game unloads and
// reloads it. A site is hooked only after its bytes are validated; a mismatch is logged at Warning and that script
// keeps the game's behaviour. Call after nevr_dll_load_hook::Install().
void Install();

}  // namespace nevr_arena_public_clock
