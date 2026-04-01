/*
 * resource_override.cpp — Hook Resource_InitFromBuffers to replace level data.
 *
 * Replaces the CComponentSpaceResource for mpl_arena_a with a modified
 * version that includes combat component types/instances merged from
 * combat maps.
 *
 * The override file is loaded from _overrides/mpl_arena_a relative to
 * the game binary directory.
 *
 * CResource struct layout (Ghidra FUN_140fa2510):
 *   +0x28: type_symbol  (CResourceID, uint64 hash)
 *   +0x38: name_symbol  (CResourceID, uint64 hash)
 *   +0x40: buf1         (primary data buffer)
 *   +0x48: buf2         (secondary data buffer)
 *   +0x50: size1        (primary buffer size)
 *   +0x58: size2        (secondary buffer size)
 *   +0x60: load_state   (uint32, 4 = loaded)
 *
 * FUN_140fa2510 takes ONLY this (RCX). Buffers are read from the struct.
 */

#include "resource_override.h"

#include <cstdio>
#include <cstring>
#include <windows.h>
#include <MinHook.h>

#include "common/globals.h"
#include "common/logging.h"

namespace {

/* Target type and name hashes (pre-computed) */
constexpr uint64_t TYPE_COMPONENT_SPACE = 0xe1273b5cac2ce869ULL;
constexpr uint64_t NAME_MPL_ARENA_A     = 0x576ed3f8428ebc4bULL;

/* Override data */
static void* g_override_buf = nullptr;
static uint64_t g_override_size = 0;
static bool g_applied = false;

/*
 * Hook AsyncResourceIOCallback (0x140fa16d0) instead of FUN_140fa2510.
 * The callback receives SIORequestCallbackData:
 *   +0x00: status (4 = success)
 *   +0x08: CResource* pointer
 *   +0x10: buffer index (0 = primary, 1 = secondary)
 *   +0x18: data buffer pointer
 *   +0x20: data size
 *
 * It writes buf/size into CResource+0x40/+0x50 then calls the deserializer.
 * We intercept here to swap the buffer BEFORE it's written to the struct.
 */
typedef void (__cdecl* AsyncIOCallback_fn)(void* callback_data);
static AsyncIOCallback_fn g_orig = nullptr;

static int g_dbg_count = 0;

static void __cdecl Hook_AsyncIOCallback(void* callback_data) {
    if (!g_applied && g_override_buf && callback_data) {
        auto cbd = reinterpret_cast<uintptr_t>(callback_data);
        void* resource = *reinterpret_cast<void**>(cbd + 0x08);
        uint32_t buf_index = *reinterpret_cast<uint32_t*>(cbd + 0x10);

        if (resource && buf_index == 0) {
            auto res = reinterpret_cast<uintptr_t>(resource);
            uint64_t name_hash = *reinterpret_cast<uint64_t*>(res + 0x38);
            uint64_t type_hash = *reinterpret_cast<uint64_t*>(res + 0x28);

            if (g_dbg_count < 30 || name_hash == NAME_MPL_ARENA_A) {
                Log(EchoVR::LogLevel::Info,
                    "[NEVR.RESOVERRIDE] IO: name=0x%016llx type=0x%016llx",
                    (unsigned long long)name_hash, (unsigned long long)type_hash);
                g_dbg_count++;
            }

            if (name_hash == NAME_MPL_ARENA_A && type_hash == TYPE_COMPONENT_SPACE) {
                uint64_t orig_size = *reinterpret_cast<uint64_t*>(cbd + 0x20);
                Log(EchoVR::LogLevel::Info,
                    "[NEVR.RESOVERRIDE] Replacing mpl_arena_a: orig_size=%llu override_size=%llu",
                    (unsigned long long)orig_size,
                    (unsigned long long)g_override_size);

                /* Replace buffer pointer and size in the callback data.
                 * AsyncResourceIOCallback will write these into the CResource struct. */
                *reinterpret_cast<void**>(cbd + 0x18) = g_override_buf;
                *reinterpret_cast<uint64_t*>(cbd + 0x20) = g_override_size;
                g_applied = true;
            }
        }
    }

    g_orig(callback_data);
}

static void* LoadFileFromDisk(const char* path, uint64_t* out_size) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return nullptr; }

    uint64_t sz = (uint64_t)li.QuadPart;
    void* buf = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { CloseHandle(hFile); return nullptr; }

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buf, (DWORD)sz, &bytesRead, NULL);
    CloseHandle(hFile);

    if (!ok || bytesRead != sz) { VirtualFree(buf, 0, MEM_RELEASE); return nullptr; }

    *out_size = sz;
    return buf;
}

} // anonymous namespace

void InstallResourceOverride() {
    /* Disabled: override data is now baked into the rebuilt package archive.
     * Keeping this function as a no-op so the call from initialize.cpp
     * doesn't need to be removed. Re-enable if hot-patching is needed. */
    return;

    /* Resolve override file path: <game_dir>/_overrides/mpl_arena_a */
    char moduleDir[MAX_PATH] = {0};
    GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, moduleDir, MAX_PATH);
    char* lastSlash = strrchr(moduleDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char overridePath[MAX_PATH];
    snprintf(overridePath, sizeof(overridePath), "%s_overrides\\mpl_arena_a", moduleDir);

    /* Load override file */
    g_override_buf = LoadFileFromDisk(overridePath, &g_override_size);
    if (!g_override_buf) {
        /* No override file — skip hook installation entirely */
        return;
    }

    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOVERRIDE] Loaded override: %s (%llu bytes, target name=0x%016llx type=0x%016llx)",
        overridePath, (unsigned long long)g_override_size,
        (unsigned long long)NAME_MPL_ARENA_A,
        (unsigned long long)TYPE_COMPONENT_SPACE);

    /* Hook AsyncResourceIOCallback @ 0x140fa16d0 */
    void* target = (void*)(EchoVR::g_GameBaseAddress + 0xFA16D0);

    if (MH_CreateHook(target, (void*)Hook_AsyncIOCallback, (void**)&g_orig) != MH_OK) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.RESOVERRIDE] MH_CreateHook failed");
        VirtualFree(g_override_buf, 0, MEM_RELEASE);
        g_override_buf = nullptr;
        return;
    }

    if (MH_EnableHook(target) != MH_OK) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.RESOVERRIDE] MH_EnableHook failed");
        MH_RemoveHook(target);
        VirtualFree(g_override_buf, 0, MEM_RELEASE);
        g_override_buf = nullptr;
        return;
    }

    Log(EchoVR::LogLevel::Info, "[NEVR.RESOVERRIDE] Hook installed on AsyncResourceIOCallback");
}
