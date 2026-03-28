/* session_unlocker — removes /session HTTP API restriction
 *
 * The game's /session endpoint (port 6721) returns err_code -6
 * ("Endpoint is restricted in this match type") in social/private lobbies.
 * This plugin patches out the match-type check so the API works in all modes.
 *
 * The HTTP handler at 0x140163254 checks:
 *   1. [rbx+0x8C10] & 0x800 — "API enabled" flag (if clear → err -2)
 *   2. [rsi] == 9 — game state == InGame (if not → err -5)
 *
 * We patch the conditional jump at 0x14016325E from JNE +6 (75 06) to
 * JMP +0x1C (EB 1C), skipping both checks and always allowing the export.
 *
 * The export functions (HTTP_Export_GameState_ToJSON at 0x140155C80,
 * HTTP_Export_PlayerData_ToJSON at 0x1401B2EA0) are read-only JSON
 * serializers — no game behavior change from allowing them in all modes.
 */

#include "nevr_plugin_interface.h"
#include "nevr_common.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[session_unlocker] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

/* Two patch sites in the HTTP /session handler:
   0x16325E: JNE +6 (flag check) — patch to JMP +6 (always pass)
   0x16326C: JE  +0E (game state == InGame check) — patch to JMP +0E (always pass) */
static constexpr uintptr_t SESSION_FLAG_CHECK = 0x16325E;
static constexpr uintptr_t SESSION_STATE_CHECK = 0x16326C;

#ifdef _WIN32
static bool PatchMemory(void* addr, const void* data, size_t len) {
    DWORD oldProtect;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(addr, data, len);
    VirtualProtect(addr, len, oldProtect, &oldProtect);
    return true;
}
#endif

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "session_unlocker";
    info.description = "Unlocks /session HTTP API in all game modes";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
#ifdef _WIN32
    uintptr_t base = ctx->base_addr;

    /* Patch 1: flag check — JNE +6 → JMP +6 (always take "allowed" branch) */
    auto* p1 = reinterpret_cast<uint8_t*>(base + SESSION_FLAG_CHECK);
    if (p1[0] == 0x75 && p1[1] == 0x06) {
        uint8_t patch[] = {0xEB, 0x06};
        if (PatchMemory(p1, patch, sizeof(patch))) {
            Log("patched flag check at +0x%x (JNE -> JMP)", (unsigned)SESSION_FLAG_CHECK);
        }
    } else {
        Log("WARN: unexpected bytes at flag check: %02x %02x (expected 75 06)", p1[0], p1[1]);
    }

    /* Patch 2: game state check — JE +0E → JMP +0E (skip "not InGame" error) */
    auto* p2 = reinterpret_cast<uint8_t*>(base + SESSION_STATE_CHECK);
    if (p2[0] == 0x74 && p2[1] == 0x0E) {
        uint8_t patch[] = {0xEB, 0x0E};
        if (PatchMemory(p2, patch, sizeof(patch))) {
            Log("patched state check at +0x%x (JE -> JMP)", (unsigned)SESSION_STATE_CHECK);
        }
    } else {
        Log("WARN: unexpected bytes at state check: %02x %02x (expected 74 0E)", p2[0], p2[1]);
    }
#else
    (void)ctx;
#endif

    Log("initialization complete");
    return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    Log("shutting down");
}
