/* SYNTHESIS -- custom tool code, not from binary */

#include "mode_patch.h"

#include <MinHook.h>
#include <cstdint>

#include "address_registry.h"
#include "nevr_common.h"
#include "plugin_log.h"

namespace combat_mod {

static constexpr uint64_t HASH_PLAYER_ARENA  = 0x118D53732E37FF19ULL;
static constexpr uint64_t HASH_PLAYER_COMBAT = 0x13983255EE1DBCBEULL;

/* ------------------------------------------------------------------ */
/* Function typedefs                                                   */
/* ------------------------------------------------------------------ */

typedef bool (__fastcall *ModeCheck_t)(uint64_t);
typedef void (__fastcall *ActorTableSelect_t)(uint64_t* a1, int a2);
typedef void (__fastcall *SetActivePlayerActor_t)(int64_t game, uint64_t actor_hash);

/* ------------------------------------------------------------------ */
/* Original function pointers (populated by MH_CreateHook)             */
/* ------------------------------------------------------------------ */

static ModeCheck_t           g_OrigIsCombat              = nullptr;
static ModeCheck_t           g_OrigIsArena               = nullptr;
static ActorTableSelect_t   g_OrigActorTableSelect       = nullptr;
static ActorTableSelect_t   g_OrigRemoteActorTableSelect = nullptr;
static SetActivePlayerActor_t g_OrigSetActivePlayerActor = nullptr;

/* ------------------------------------------------------------------ */
/* Hook implementations                                                */
/* ------------------------------------------------------------------ */

/* Passthrough -- instrumentation point for future combat-mode overrides. */
static bool __fastcall Hook_IsCombat(uint64_t hash) {
    return g_OrigIsCombat(hash);
}

/* Passthrough -- instrumentation point for future arena-mode overrides. */
static bool __fastcall Hook_IsArena(uint64_t hash) {
    return g_OrigIsArena(hash);
}

/* Force combat chassis (a2=1) for local player spawn. */
static void __fastcall Hook_ActorTableSelect(uint64_t* a1, int a2) {
    combat_mod::PluginLog(
        "[mode_patch] LocalActorTableSelect: a2=%d -> forced to 1 (combat)", a2);
    g_OrigActorTableSelect(a1, 1);
}

/* Force combat chassis (a2=1) for remote player spawn. */
static void __fastcall Hook_RemoteActorTableSelect(uint64_t* a1, int a2) {
    combat_mod::PluginLog(
        "[mode_patch] RemoteActorTableSelect: a2=%d -> forced to 1 (combat)", a2);
    g_OrigRemoteActorTableSelect(a1, 1);
}

/* Block player_arena hash, replace with player_combat. */
static void __fastcall Hook_SetActivePlayerActor(int64_t game, uint64_t actor_hash) {
    if (actor_hash == HASH_PLAYER_ARENA) {
        combat_mod::PluginLog(
            "[mode_patch] BLOCKED SetActivePlayerActor(player_arena) -> player_combat");
        actor_hash = HASH_PLAYER_COMBAT;
    }
    g_OrigSetActivePlayerActor(game, actor_hash);
}

/* ------------------------------------------------------------------ */
/* Helper: create + enable a single MinHook, append target to hooks     */
/* ------------------------------------------------------------------ */

static bool InstallHook(void* target, void* detour, void** original,
                         const char* name, uint64_t va,
                         std::vector<void*>& hooks) {
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        combat_mod::PluginLog(
            "[mode_patch] MH_CreateHook failed for %s: %d", name, status);
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        combat_mod::PluginLog(
            "[mode_patch] MH_EnableHook failed for %s: %d", name, status);
        return false;
    }

    hooks.push_back(target);
    combat_mod::PluginLog(
        "[mode_patch] Hooked %s @ VA 0x%llX", name,
        static_cast<unsigned long long>(va));
    return true;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

void InstallModePatch(uintptr_t base, std::vector<void*>& hooks) {
    combat_mod::PluginLog( "[mode_patch] Installing mode patch hooks");

    InstallHook(
        nevr::ResolveVA(base, nevr::addresses::VA_IS_COMBAT_GAME_TYPE),
        reinterpret_cast<void*>(&Hook_IsCombat),
        reinterpret_cast<void**>(&g_OrigIsCombat),
        "IsCombatGameType", nevr::addresses::VA_IS_COMBAT_GAME_TYPE, hooks);

    InstallHook(
        nevr::ResolveVA(base, nevr::addresses::VA_IS_ARENA_GAME_TYPE),
        reinterpret_cast<void*>(&Hook_IsArena),
        reinterpret_cast<void**>(&g_OrigIsArena),
        "IsArenaGameType", nevr::addresses::VA_IS_ARENA_GAME_TYPE, hooks);

    InstallHook(
        nevr::ResolveVA(base, nevr::addresses::VA_LOCAL_ACTOR_TABLE_SELECT),
        reinterpret_cast<void*>(&Hook_ActorTableSelect),
        reinterpret_cast<void**>(&g_OrigActorTableSelect),
        "LocalActorTableSelect", nevr::addresses::VA_LOCAL_ACTOR_TABLE_SELECT, hooks);

    InstallHook(
        nevr::ResolveVA(base, nevr::addresses::VA_REMOTE_ACTOR_TABLE_SELECT),
        reinterpret_cast<void*>(&Hook_RemoteActorTableSelect),
        reinterpret_cast<void**>(&g_OrigRemoteActorTableSelect),
        "RemoteActorTableSelect", nevr::addresses::VA_REMOTE_ACTOR_TABLE_SELECT, hooks);

    InstallHook(
        nevr::ResolveVA(base, nevr::addresses::VA_SET_ACTIVE_PLAYER_ACTOR),
        reinterpret_cast<void*>(&Hook_SetActivePlayerActor),
        reinterpret_cast<void**>(&g_OrigSetActivePlayerActor),
        "SetActivePlayerActor", nevr::addresses::VA_SET_ACTIVE_PLAYER_ACTOR, hooks);
}

} // namespace combat_mod
