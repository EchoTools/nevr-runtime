/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <vector>

namespace combat_mod {

/*
 * InstallModePatch -- hook IsCombatGameType, IsArenaGameType,
 * LocalActorTableSelect, RemoteActorTableSelect, and SetActivePlayerActor
 * to force combat chassis in arena game modes.
 *
 * IsCombat/IsArena are passthrough hooks (instrumentation points for future
 * overrides). ActorTableSelect hooks force a2=1 (combat chassis index).
 * SetActivePlayerActor blocks the player_arena hash and replaces it with
 * player_combat.
 *
 * Appends each hook target to `hooks` for shutdown cleanup.
 */
void InstallModePatch(uintptr_t base, std::vector<void*>& hooks);

} // namespace combat_mod
