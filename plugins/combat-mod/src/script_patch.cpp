/*
 * script_patch.cpp -- In-memory patching of engine script DLLs.
 *
 * Hooks the engine's custom DLL loader (VA_LOAD_SCRIPT_DLL). When a
 * script module loads, identifies it by UUID in the file path and applies
 * byte patches to the loaded image. Patches are applied unconditionally
 * on every load -- no level gating.
 *
 * Ported from echovr_combat_mod/scriptpatch.h. SEH (__try/__except)
 * replaced with nevr::SafeMemcmp / nevr::SafeMemcpy for MinGW compat.
 */

#include "script_patch.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>

#include "safe_memory.h"
#include "combat_log.h"
#include "nevr_common.h"
#include "address_registry.h"

namespace {

struct BytePatch {
    uint32_t fileOffset;
    uint8_t  orig[8];
    uint8_t  patch[8];
    uint8_t  len;
};

// ---------------------------------------------------------------------------
// Patch tables -- byte-identical to original scriptpatch.h
// ---------------------------------------------------------------------------

static const BytePatch kStreamingPatches[] = {
    {0x2BD8, {0xB7,0x44,0x3D,0xD3,0xA6,0x00,0xAA,0x6D}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x2EAF, {0xB7,0x44,0x3D,0xD3,0xA6,0x00,0xAA,0x6D}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x3078, {0xB7,0x44,0x3D,0xD3,0xA6,0x00,0xAA,0x6D}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x31DF, {0xB7,0x44,0x3D,0xD3,0xA6,0x00,0xAA,0x6D}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x334F, {0xB7,0x44,0x3D,0xD3,0xA6,0x00,0xAA,0x6D}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x34BF, {0xB7,0x44,0x3D,0xD3,0xA6,0x00,0xAA,0x6D}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x2CF8, {0x26,0x45,0x2B,0xFC,0xF7,0x77,0x99,0xCB}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x31F5, {0x26,0x45,0x2B,0xFC,0xF7,0x77,0x99,0xCB}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x3365, {0x26,0x45,0x2B,0xFC,0xF7,0x77,0x99,0xCB}, {0xBA,0xA2,0x28,0x52,0xCF,0xDE,0x3E,0x81}, 8},
    {0x2E9F, {0x81}, {0xC3}, 1},
    {0x34AF, {0x81}, {0xC3}, 1},
};
static constexpr int kStreamingPatchCount =
    sizeof(kStreamingPatches) / sizeof(kStreamingPatches[0]);

// Arena spectator script (12220d647ad3f5d3.dll) -- disable "force arena chassis".
// Patches sub_180006CC0 (conditional: is combat player in arena?) to return false.
// Prevents "Player Spawned in Arena as Combat Player - Switching to Arena".
static const BytePatch kArenaChassisPatches[] = {
    // File offset 0x60C0: sub_180006CC0 -- patch to: xor eax, eax; ret; nop*4
    {0x60C0, {0x48,0x81,0xEC,0x78,0x03,0x00,0x00}, {0x31,0xC0,0xC3,0x90,0x90,0x90,0x90}, 7},
};
static constexpr int kArenaChassisCount =
    sizeof(kArenaChassisPatches) / sizeof(kArenaChassisPatches[0]);

static const BytePatch kEventPatches[] = {
    {0x9640, {0x65}, {0x58}, 1},
    {0x96A0, {0x65}, {0x58}, 1},
    {0x96B0, {0x65}, {0x58}, 1},
    {0x9758, {0x65}, {0x58}, 1},
    {0x9770, {0x65}, {0x58}, 1},
};
static constexpr int kEventPatchCount =
    sizeof(kEventPatches) / sizeof(kEventPatches[0]);

// ---------------------------------------------------------------------------
// PatchDLL -- apply a table of byte patches to a loaded script image.
//
// Translates file offsets to virtual addresses via the PE section table,
// validates original bytes with SafeMemcmp, and writes with SafeMemcpy.
// Returns the number of patches applied, or -1 on PE header validation failure.
// ---------------------------------------------------------------------------

static int PatchDLL(uintptr_t base, const BytePatch* patches, int count) {
    // Validate DOS header
    const uint8_t kDosSig[] = {0x4D, 0x5A};  // "MZ"
    if (!nevr::SafeMemcmp(reinterpret_cast<const void*>(base), kDosSig, 2))
        return -1;

    // Read e_lfanew (offset to NT headers) -- at offset 0x3C in DOS header
    int32_t e_lfanew = 0;
    if (!nevr::SafeMemcpy(&e_lfanew, reinterpret_cast<const void*>(base + 0x3C),
                           sizeof(e_lfanew)))
        return -1;

    uintptr_t ntAddr = base + static_cast<uint32_t>(e_lfanew);

    // Validate NT signature ("PE\0\0" = 0x00004550)
    const uint8_t kPeSig[] = {0x50, 0x45, 0x00, 0x00};
    if (!nevr::SafeMemcmp(reinterpret_cast<const void*>(ntAddr), kPeSig, 4))
        return -1;

    // Read NumberOfSections from COFF header (offset +6 from NT start)
    uint16_t numSec = 0;
    if (!nevr::SafeMemcpy(&numSec, reinterpret_cast<const void*>(ntAddr + 6),
                           sizeof(numSec)))
        return -1;

    // Read SizeOfOptionalHeader (offset +20 from NT start)
    uint16_t optHdrSize = 0;
    if (!nevr::SafeMemcpy(&optHdrSize, reinterpret_cast<const void*>(ntAddr + 20),
                           sizeof(optHdrSize)))
        return -1;

    // Section headers start after the optional header
    // NT signature (4) + COFF header (20) + optional header
    uintptr_t secBase = ntAddr + 4 + 20 + optHdrSize;

    int applied = 0;
    for (int i = 0; i < count; i++) {
        uintptr_t addr = 0;

        // Walk sections to translate file offset -> virtual address
        for (uint16_t s = 0; s < numSec; s++) {
            // IMAGE_SECTION_HEADER is 40 bytes each
            uintptr_t sh = secBase + 40ULL * s;

            // PointerToRawData at offset +20, SizeOfRawData at +16, VirtualAddress at +12
            uint32_t rawStart = 0, rawSize = 0, virtAddr = 0;
            if (!nevr::SafeMemcpy(&rawStart, reinterpret_cast<const void*>(sh + 20), 4))
                continue;
            if (!nevr::SafeMemcpy(&rawSize, reinterpret_cast<const void*>(sh + 16), 4))
                continue;
            if (!nevr::SafeMemcpy(&virtAddr, reinterpret_cast<const void*>(sh + 12), 4))
                continue;

            uint32_t rawEnd = rawStart + rawSize;
            if (patches[i].fileOffset >= rawStart && patches[i].fileOffset < rawEnd) {
                addr = base + virtAddr + (patches[i].fileOffset - rawStart);
                break;
            }
        }
        if (!addr) continue;

        // Validate original bytes before patching
        if (!nevr::SafeMemcmp(reinterpret_cast<const void*>(addr),
                               patches[i].orig, patches[i].len))
            continue;

        // Apply the patch
        DWORD oldProt;
        if (VirtualProtect(reinterpret_cast<void*>(addr), patches[i].len,
                           PAGE_EXECUTE_READWRITE, &oldProt)) {
            nevr::SafeMemcpy(reinterpret_cast<void*>(addr), patches[i].patch,
                              patches[i].len);
            VirtualProtect(reinterpret_cast<void*>(addr), patches[i].len,
                           oldProt, &oldProt);
            applied++;
        }
    }
    return applied;
}

// ---------------------------------------------------------------------------
// Hook for engine script DLL loader
// ---------------------------------------------------------------------------

using LoadScriptDLL_t = int64_t(__fastcall*)(const char* path);
static LoadScriptDLL_t Orig_LoadScriptDLL = nullptr;

static int64_t __fastcall Hook_LoadScriptDLL(const char* path) {
    int64_t handle = Orig_LoadScriptDLL(path);
    if (handle && path) {
        if (strstr(path, "f808c072bb49da47")) {
            int n = PatchDLL(static_cast<uintptr_t>(handle),
                             kStreamingPatches, kStreamingPatchCount);
            combat_mod::PluginLog(
                "PATCHED streaming script (%d/%d) @ 0x%llX",
                n, kStreamingPatchCount, static_cast<unsigned long long>(handle));
        }
        if (strstr(path, "d44f62ba114f0cde")) {
            int n = PatchDLL(static_cast<uintptr_t>(handle),
                             kEventPatches, kEventPatchCount);
            combat_mod::PluginLog(
                "PATCHED event script (%d/%d) @ 0x%llX",
                n, kEventPatchCount, static_cast<unsigned long long>(handle));
        }
        if (strstr(path, "12220d647ad3f5d3")) {
            int n = PatchDLL(static_cast<uintptr_t>(handle),
                             kArenaChassisPatches, kArenaChassisCount);
            combat_mod::PluginLog(
                "PATCHED arena chassis script (%d/%d) @ 0x%llX",
                n, kArenaChassisCount, static_cast<unsigned long long>(handle));
        }
    }
    return handle;
}

} // anonymous namespace

namespace combat_mod {

void InstallScriptPatch(uintptr_t base, nevr::HookManager& hooks) {
    void* target = nevr::ResolveVA(base, nevr::addresses::VA_LOAD_SCRIPT_DLL);

    if (MH_CreateHook(target, reinterpret_cast<void*>(Hook_LoadScriptDLL),
                       reinterpret_cast<void**>(&Orig_LoadScriptDLL)) == MH_OK) {
        MH_EnableHook(target);
        hooks.Track(target);
        combat_mod::PluginLog(
            "Script DLL loader hooked — patches apply on load");
    } else {
        combat_mod::PluginLog(
            "Failed to hook script DLL loader");
    }
}

} // namespace combat_mod
