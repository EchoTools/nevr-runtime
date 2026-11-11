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

// A filter on every call of the three setters, ahead of the re-fire tracking: it gets the slot (0 weapon, 1 tac mod,
// 2 ordnance), the component, the handle and the value, and returns the value the setter is called with. The combat module's
// LOADOUTS rule is the one user (runtime/combat): one owner per function, so it rides this hook instead of detouring the setters a
// second time. Null removes it. Returns false when the setters are not hooked (a pinned check failed), so the caller says so.
using SlotFilter = std::uint64_t (*)(int slot, void* component, std::uint32_t handle, std::uint64_t value);
bool SetSlotFilter(SlotFilter filter);

// The setter itself (the game's code behind the hook), for a caller that sets a slot without the filter and without the
// re-fire tracking. Null when the setters are not hooked.
using SetterFn = void (*)(void* component, std::uint32_t handle, std::uint64_t value);
SetterFn OriginalSetter(int slot);

// Runs the re-fires that are due. Called every tick from the game loop's thread (frame/tick.cpp), because the handlers send
// events into the game's scripts.
void OnFrame();

}  // namespace nevr_equipment_refire
