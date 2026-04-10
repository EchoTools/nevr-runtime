/*
 * combat-2d plugin — PC/2D (non-VR) mode fixes for EchoVR.
 *
 * Three byte patches, each with prologue validation:
 *   1. Hand position fix: NOP the JZ that skips hand transform computation
 *      in 2D mode, so hand positions replicate correctly to other players.
 *   2. VR flag fix: Force bit 4 in outgoing player-state flags, telling
 *      receivers to apply hand offsets via IK solver.
 *   3. AFK fix: Always pass afk=0 to the status setter, preventing
 *      inactivity kick in PC mode (VR tracking inactive = always AFK).
 *
 * Client-only plugin — no-ops on servers.
 */

#include "common/nevr_plugin_interface.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "nevr_common.h"
#include "address_registry.h"

namespace {

void PluginLog(const char* fmt, ...) {
    std::fprintf(stderr, "[NEVR.2D] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

uintptr_t g_base = 0;

bool PatchBytes(uintptr_t rva, const void* bytes, size_t len) {
    uintptr_t addr = g_base + rva;
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), len,
                        PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    memcpy(reinterpret_cast<void*>(addr), bytes, len);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
    VirtualProtect(reinterpret_cast<void*>(addr), len, oldProt, &oldProt);
    return true;
}

bool VerifyAndPatch(uintptr_t rva, const uint8_t* expected, const uint8_t* patch,
                    size_t len, const char* name) {
    const uint8_t* site = reinterpret_cast<const uint8_t*>(g_base + rva);
    if (memcmp(site, expected, len) != 0) {
        PluginLog("%s: byte mismatch at RVA 0x%llX — wrong binary?",
                  name, (unsigned long long)rva);
        return false;
    }
    if (!PatchBytes(rva, patch, len)) {
        PluginLog("%s: FAILED to patch at RVA 0x%llX", name, (unsigned long long)rva);
        return false;
    }
    PluginLog("%s: applied at RVA 0x%llX", name, (unsigned long long)rva);
    return true;
}

} // anonymous namespace

extern "C" {

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo() {
    NvrPluginInfo info = {};
    info.name = "combat_2d";
    info.description = "PC/2D mode fixes — hand position, VR flag, AFK prevention";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
    return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    /* Client-only — no-op on servers */
    if (ctx->flags & NEVR_HOST_IS_SERVER) {
        PluginLog("Server mode, skipping 2D patches");
        return 0;
    }

    g_base = ctx->base_addr;
    int applied = 0;

    /* 1. Hand position fix: NOP the 6-byte JZ at 0xD6ECC3 */
    {
        constexpr uintptr_t rva = nevr::addresses::VA_HAND_POSITION_JZ - 0x140000000;
        const uint8_t expected[] = {0x0F, 0x84, 0x5B, 0x01, 0x00, 0x00};
        const uint8_t patch[]    = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        if (VerifyAndPatch(rva, expected, patch, 6, "Hand position fix"))
            applied++;
    }

    /* 2. VR flag fix: Replace "and ecx,10h; xor eax,ecx" with "or eax,10h; nop; nop" */
    {
        constexpr uintptr_t rva = nevr::addresses::VA_VR_FLAG_PATCH - 0x140000000;
        const uint8_t expected[] = {0x83, 0xE1, 0x10, 0x33, 0xC1};
        const uint8_t patch[]    = {0x83, 0xC8, 0x10, 0x90, 0x90};
        if (VerifyAndPatch(rva, expected, patch, 5, "VR flag fix"))
            applied++;
    }

    /* 3. AFK fix: Replace "mov r9d,ebp" with "xor r9d,r9d" */
    {
        constexpr uintptr_t rva = nevr::addresses::VA_AFK_PATCH - 0x140000000;
        const uint8_t expected[] = {0x44, 0x8B, 0xCD};
        const uint8_t patch[]    = {0x45, 0x33, 0xC9};
        if (VerifyAndPatch(rva, expected, patch, 3, "AFK fix"))
            applied++;
    }

    PluginLog("Init complete (%d/3 patches applied)", applied);
    return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown() {
    /* Byte patches can't be cleanly reverted without saving originals,
     * and the game is shutting down anyway. */
    PluginLog("Shutdown");
}

} /* extern "C" */
