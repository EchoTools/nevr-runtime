/*
 * plugin.cpp — debug_lockout: verify whether the early-quit lockout state
 * is correctly received by the game client.
 *
 * Hooks CR14LocalPlayerCS::ApplyEarlyQuitState @ 0x1401ae2e0. Before the
 * original runs it force-injects an active level-3 lockout (penalty_level=3,
 * penalty_ts = now + 3600 at BOTH +0x64820 and +0x64828, num_early_quits=16)
 * so the client behaves as if nakama had sent one — no server needed. Then it
 * calls the original and dumps the values left behind, to verify the state
 * survives.
 *
 * ReVault disassembly:
 *   0x1401ae2f0: CMP [RCX + 0x64820], -1 ... JGE 0x1401ae41d — early-exit:
 *                if +0x64820 already holds a timestamp >= the incoming one,
 *                the function writes NOTHING, so injected state survives.
 *   0x1401ae339: MOV [RCX + 0x64820], RDX   — stores the incoming penalty_ts
 *   0x1401ae372: MOV qword ptr [RCX + 0x64828], -0x1  — stores -1 sentinel
 *
 * The countdown reader at 0x140d962c1 and the per-frame expiry check at
 * 0x1401bc5d0 both read +0x64828. If that field stays -1, the lockout never
 * activates client-side — that is the bug this plugin exists to confirm.
 */

#include "extension/plugin_interface.h"
#include "plugin_logger.h"
#include "hook_manager.h"
#include "nevr_common.h"

#include <cstdint>
#include <ctime>

NEVR_DEFINE_PLUGIN_LOG("[debug_lockout]")

/*
 * Hook target: CR14LocalPlayerCS::ApplyEarlyQuitState @ 0x1401ae2e0.
 * Prologue: PUSH RDI; SUB RSP, 0x20 — six bytes, MinHook-safe.
 * VA and prologue bytes sourced from ReVault, validated at init time.
 */
static constexpr uint64_t  kHookTargetVA    = 0x1401ae2e0;
static constexpr uint8_t   kPrologue[]      = {0x40, 0x57, 0x48, 0x83, 0xec, 0x20};

/*
 * Function signature (__fastcall, first arg in RCX, second in RDX).
 */
using ApplyEarlyQuitStateFn = void (__fastcall *)(void* playerCS, uint64_t penalty_ts);

static ApplyEarlyQuitStateFn g_original = nullptr;
static nevr::HookManager     g_hooks;

/*
 * Detour — force-inject an active lockout BEFORE the original runs, then call
 * the original and dump whatever survives. The original early-exits without
 * writing when +0x64820 already holds a timestamp >= the incoming penalty_ts
 * (see the header comment), so in the normal case — server sends no lockout,
 * i.e. -1 — the injected state survives untouched.
 */
static void __fastcall Detour_ApplyEarlyQuitState(void* playerCS, uint64_t penalty_ts)
{
    uint64_t forced_ts = static_cast<uint64_t>(time(nullptr)) + 3600;

    *(uint8_t*)((uint8_t*)playerCS + 0x64844)  = 3;         /* penalty_level    */
    *(uint64_t*)((uint8_t*)playerCS + 0x64820) = forced_ts; /* stored penalty_ts */
    *(uint64_t*)((uint8_t*)playerCS + 0x64828) = forced_ts; /* consumed penalty_ts */
    *(int32_t*)((uint8_t*)playerCS + 0x64830)  = 16;        /* num_early_quits  */

    PluginLog("FORCE-LOCKOUT: injected penalty_level=3 penalty_ts=%llu (+3600s) at +0x64820 and +0x64828",
              static_cast<unsigned long long>(forced_ts));

    g_original(playerCS, penalty_ts);

    uint64_t field_64820 = *(uint64_t*)((uint8_t*)playerCS + 0x64820);
    uint64_t field_64828 = *(uint64_t*)((uint8_t*)playerCS + 0x64828);

    PluginLog("LOCKOUT state applied:");
    PluginLog("  incoming penalty_ts = %llu (%s)",
              (unsigned long long)penalty_ts,
              penalty_ts == (uint64_t)-1 ? "NONE" : "ACTIVE");
    PluginLog("  +0x64820 (stored)   = %llu (%s)",
              (unsigned long long)field_64820,
              field_64820 == (uint64_t)-1 ? "NONE" : (field_64820 > penalty_ts ? "FUTURE" : "EXPIRED"));
    PluginLog("  +0x64828 (consumed)  = %llu (%s)",
              (unsigned long long)field_64828,
              field_64828 == (uint64_t)-1 ? "SENTINEL (-1)" : "VALUE SET");
    if (field_64828 == (uint64_t)-1) {
        PluginLog("  *** BUG CONFIRMED: +0x64828 is -1, lockout will never activate ***");
    }
}

/*
 * ── PLUGIN LIFECYCLE EXPORTS ───────────────────────────────────────────
 */

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void)
{
    NvrPluginInfo info = {};
    info.name          = "nevr_debug_lockout";
    info.description   = "Debug: force-injects an active level-3 lockout and dumps the resulting state";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void)
{
    return NEVR_PLUGIN_API_VERSION;
}

/*
 * Force-injects penalty state into the client, so it must declare ALTERS_RULES
 * (loads in the highest-risk band) rather than OBSERVES_ONLY. The detour is a
 * plain pass-through through the host's shared MinHook, so HOOKS_ENGINE does
 * not apply (see the example plugin's documentation of that bit).
 */
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void)
{
    return NEVR_PLUGIN_CAP_ALTERS_RULES;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx)
{
    if (ctx == nullptr) {
        PluginLog("init: context is null");
        return -1;
    }

    PluginLog("init: base=0x%llx flags=0x%x game_state=%u",
              static_cast<unsigned long long>(ctx->base_addr),
              static_cast<unsigned int>(ctx->flags),
              static_cast<unsigned int>(ctx->game_state));

    /* Step 1: Initialize MinHook. ALREADY_INITIALIZED is harmless. */
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        PluginLog("hook: MH_Initialize failed: %s",
                  MH_StatusToString(mhStatus));
        return -1;
    }

    /* Step 2: Resolve the VA. nullptr = no game binary / VA out of range. */
    void* target = nevr::ResolveVA_Checked(ctx->base_addr, kHookTargetVA);
    if (target == nullptr) {
        PluginLog("hook: VA 0x%llx not valid in this process —"
                  " no game binary loaded, or VA out of range;"
                  " plugin continues without this hook",
                  static_cast<unsigned long long>(kHookTargetVA));
        return 0; /* NOT an error: plugin works without the game */
    }

    /* Step 3: Validate the prologue — wrong bytes = wrong game version. */
    if (!nevr::ValidatePrologue(target, kPrologue, sizeof(kPrologue))) {
        PluginLog("hook: prologue mismatch at 0x%llx —"
                  " wrong game version? skipping this hook",
                  static_cast<unsigned long long>(kHookTargetVA));
        return 0;
    }

    /* Step 4: Create and enable the hook via HookManager. */
    MH_STATUS s = g_hooks.CreateAndEnable(
        target,
        reinterpret_cast<void*>(&Detour_ApplyEarlyQuitState),
        reinterpret_cast<void**>(&g_original));

    if (s != MH_OK) {
        PluginLog("hook: CreateAndEnable failed: %s",
                  MH_StatusToString(s));
        return -1;
    }

    PluginLog("hook: installed on ApplyEarlyQuitState @ 0x%llx"
              " (%zu hooks tracked)",
              static_cast<unsigned long long>(kHookTargetVA),
              g_hooks.count());

    return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void)
{
    PluginLog("shutdown: removing %zu hooks", g_hooks.count());
    g_hooks.RemoveAll();
    PluginLog("shutdown: complete");
}
