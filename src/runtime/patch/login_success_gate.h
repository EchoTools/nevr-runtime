#pragma once
// CR15NetGame::LogInSuccess writes social.community_values_version = 1 into the player's profile only when bit 9 of
// the netgame flags qword is set. This hook forces that bit for the duration of the one call, so the write happens on
// every login in a server, headless, windowed or offline run, and restores the word afterwards. The rules are in
// login_success_gate_rules.h.

#include <cstdint>

namespace nevr_login_success_gate {

// Detours LogInSuccess at gameBase + 0x17F630 after its prologue is validated. Returns false when the hook is not in
// place (the prologue did not match, or the detour did not install); the cause is logged at Warning. Call once per
// run, from boot, for a server, headless, windowed or offline run. requestedBy names that mode in the install line.
bool Install(std::uintptr_t gameBase, const char* requestedBy);

// Detours `target` (LogInSuccess, or a stand-in with the same signature in a test) with the hook. Returns false when
// PatchDetour did not install it.
bool AttachTo(void* target, const char* requestedBy);

}  // namespace nevr_login_success_gate
