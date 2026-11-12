#pragma once
// A "clear all new unlocks" made while a profile update is unacknowledged is sent after the acknowledgement (#2410). Hooks the game's clear
// (0x1401a06c0); the game's clear runs as it always did, and when an update was in flight a per-frame poll sets the user's dirty bit once the
// acknowledgement has reset the in-flight buffer, so the game's live SaveClientProfileChanges (in the platform module) sends the clear itself.
// Decisions: new_unlocks_clear_rules.h. The Quest build would call the same rules with its own addresses (the table in that header).

#include <cstdint>

namespace nevr_new_unlocks_clear {

// Validates the clear and the emptiness test by their first 16 bytes, then hooks the clear. A mismatch is logged and nothing is installed.
// Call after the hooking engine is up; logs through the boot tee.
void Install(std::uintptr_t gameBase);

// Once per frame, on the game loop's thread (frame/tick.cpp): re-arms a held clear whose update has been acknowledged. One pointer test
// while nothing is held.
void OnFrame();

}  // namespace nevr_new_unlocks_clear
