// ability_probe.h: test-only (mingw-scenario preset, NEVR_SCENARIO_CONTROL) probe of the stock tac-mod use path (#2365, the
// design's first check). Never in a release build: every line it writes carries [NEVR.SCENARIO], which
// tools/verify_scenario_control_absent.py refuses in a release DLL.
//
// On a server it hooks, read-only, the game's receiver of SR15NetAbilityActivate (CR15NetGame::AbilityActivateCB, echovr.exe
// 0x140150f50) and of SR15NetPlayerEquipmentUpdate (ReceivePlayerEquipmentUpdate 0x140d37300), logs what each received
// and calls the game's function unchanged. For each use it resolves the user handle the way the callback does
// (NetGame+0x138/+0x13a) and reads that player's equipment record +0x20 (the selected tac mod).
//
// On a client, the scenario op `ability_activate` makes the game send the same message the stock cooldown node sends
// (CR15NetAbilityCooldownNode::Enter 0x140dd8f40 after its "Cooldown node activate" line): the game's own
// Broadcast<SR15NetAbilityActivate> (0x14012a010) with the local player's handle, to GetAuthorityPeer's answer. It proves
// the server half only; whether the stock node runs and sends on a real client against a -server is not shown by it.
#pragma once

#include <string>

namespace nevr_ability_probe {

/// Installs the server's read-only hooks (a client installs nothing). Boot, after the game's first original call.
void Install();

/// Client, game thread: sends one SR15NetAbilityActivate {local handle, active} to the authority peer through the game's
/// own send. "" when sent, else why not.
std::string SendActivateForTest(bool active);

}  // namespace nevr_ability_probe
