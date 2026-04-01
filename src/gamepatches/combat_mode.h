#pragma once

/// Install hooks and patches to force combat mode in arena:
/// - IsCombatGameType -> always true
/// - IsArenaGameType -> always false
/// - PlayerInit -> set combat flag on player component
/// - Equip chassis check: JZ -> JMP (bypass weapon stripping)
/// - Weapon guard jumps: NOPed
void InstallCombatMode();
