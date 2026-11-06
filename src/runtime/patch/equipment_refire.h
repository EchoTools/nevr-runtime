#pragma once
// The equipment change the game drops is applied again (#2121). Hooks the three setters of CR15NetPlayerEquipmentCS (weapon
// 0x140d498e0, tac mod 0x140d43f00, ordnance 0x140d45990); after the local player's last change of a slot, once the slot has been
// still for nevr_equipment_refire::kSettleMs, the slot's change handler (0x140cec740 / 0x140ceaaa0 / 0x140ceb720) runs once more.
// Decisions: equipment_refire_rules.h. The Quest twin is quest/combat/equip_refire.cpp.

#include <cstdint>

namespace nevr_equipment_refire {

// Validates the three setters, the three handlers and the two helpers the local-player check calls by their bytes, then hooks
// the setters. A mismatch is logged and nothing is installed. Call after the hooking engine is up; logs through the boot tee.
void Install(std::uintptr_t gameBase);

// Runs the re-fires that are due. Called every tick from the game loop's thread (frame/tick.cpp), because the handlers send
// events into the game's scripts.
void OnFrame();

}  // namespace nevr_equipment_refire
