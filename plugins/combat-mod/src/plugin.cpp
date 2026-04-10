/*
 * plugin.cpp — NvrPluginInterface lifecycle for the combat-mod plugin.
 *
 * Init order (matching echovr_combat_mod.dll):
 *   1. ScriptPatch — hooks engine DLL loader (immediate, no level gate)
 *   2. ModePatch — forces combat mode checks + chassis selection
 *   3. StartVisible — hooks model init for clone visibility
 *   4. LevelDetect — hooks level load
 *   5. LevelOffset — hooks sublevel position offset
 *   6. CombatEarly — team change hook (fires during lobby)
 *
 * Level-gated hooks (installed on state change to combat level):
 *   7. CombatLevelHooks — respawn, net replication, PlayerInit
 *
 * Per-frame:
 *   - F9 polling (~20Hz throttle)
 *   - Script DLL discovery (deferred, every ~2s)
 *   - Pending offset application
 */

#include <MinHook.h>

#include "common/nevr_plugin_interface.h"
#include "plugin_log.h"

#include "script_patch.h"
#include "mode_patch.h"
#include "start_visible.h"
#include "level_detect.h"
#include "combat_system.h"

#include <vector>
#include <cstdint>

namespace {

std::vector<void*> g_ownedHooks;
uintptr_t g_base = 0;
bool g_levelHooksInstalled = false;

} // anonymous namespace

extern "C" {

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo() {
    NvrPluginInfo info = {};
    info.name = "combat_mod";
    info.description = "Combat mode for arena levels — chassis swap, weapons, respawn, F9 toggle";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    g_base = ctx->base_addr;

    combat_mod::PluginLog( "Initializing combat-mod plugin");

    /* Phase 1: Immediate hooks (no level gate) */
    combat_mod::InstallScriptPatch(g_base, g_ownedHooks);
    combat_mod::InstallModePatch(g_base, g_ownedHooks);
    combat_mod::InstallStartVisible(g_base, g_ownedHooks);
    combat_mod::InstallLevelDetect(g_base, g_ownedHooks);
    combat_mod::InstallLevelOffset(g_base, g_ownedHooks);
    combat_mod::InstallCombatEarly(g_base, g_ownedHooks);

    combat_mod::PluginLog( "Init complete (%zu hooks installed)",
        g_ownedHooks.size());

    return 0;  /* success */
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext* ctx) {
    /* Install level-gated hooks once combat level is detected */
    if (!g_levelHooksInstalled && combat_mod::IsArenaCombat()) {
        combat_mod::InstallCombatLevelHooks(g_base, g_ownedHooks);
        g_levelHooksInstalled = true;
        combat_mod::PluginLog(
            "mpl_arenacombat detected — level hooks installed (%zu total)",
            g_ownedHooks.size());
    }

    /* Per-frame combat update (F9, script DLL discovery, offset) */
    if (g_levelHooksInstalled) {
        combat_mod::CombatOnFrame();
    }
}

NEVR_PLUGIN_API void NvrPluginOnGameStateChange(
    const NvrGameContext* ctx, uint32_t old_state, uint32_t new_state)
{
    /* Nothing needed currently — level detection is via hook, not state */
}

NEVR_PLUGIN_API void NvrPluginShutdown() {
    combat_mod::PluginLog( "Shutting down (%zu hooks to remove)",
        g_ownedHooks.size());

    /* Disable and remove only this plugin's hooks */
    for (void* target : g_ownedHooks) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    g_ownedHooks.clear();
    g_levelHooksInstalled = false;

    combat_mod::PluginLog( "Shutdown complete");
}

} /* extern "C" */
