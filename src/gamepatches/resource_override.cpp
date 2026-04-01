/*
 * resource_override.cpp — Hook AsyncResourceIOCallback to replace game resources.
 *
 * Provides a registry-based system for overriding game resources by their
 * type and name symbol hashes. Overrides can be either embedded data (compiled
 * into the DLL) or loaded from disk at runtime.
 *
 * Hook target: AsyncResourceIOCallback @ 0x140fa16d0
 * The callback receives SIORequestCallbackData:
 *   +0x00: status (4 = success)
 *   +0x08: CResource* pointer
 *   +0x10: buffer index (0 = primary, 1 = secondary)
 *   +0x18: data buffer pointer
 *   +0x20: data size
 *
 * CResource struct layout:
 *   +0x28: type_symbol  (uint64 hash)
 *   +0x38: name_symbol  (uint64 hash)
 */

#include "resource_override.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>
#include <MinHook.h>

#include "common/globals.h"
#include "common/logging.h"

/* Embedded assets */
#include "splash_texture.h"

namespace {

/* ── Override registry ────────────────────────────────────────────── */

struct ResourceOverride {
    uint64_t type_hash;     /* 0 = match any type */
    uint64_t name_hash;
    const void* data;       /* embedded data (not owned) or VirtualAlloc'd (owned) */
    uint64_t size;
    bool owned;             /* true if data was allocated and should be freed */
    const char* label;      /* for logging */
    bool applied;           /* set to true after first replacement */
};

static std::vector<ResourceOverride>* g_overrides = nullptr;
static uint64_t g_total_hits = 0;

/* ── Known asset hashes ───────────────────────────────────────────── */

constexpr uint64_t TYPE_DDS_TEXTURE     = 0xbeac1969cb7b8861ULL;
constexpr uint64_t NAME_SPLASH_TEXTURE  = 0x1f72c5e0fc9fc8c0ULL;

/* ── Hook ─────────────────────────────────────────────────────────── */

typedef void (__cdecl* AsyncIOCallback_fn)(void* callback_data);
static AsyncIOCallback_fn g_orig = nullptr;
static void* g_hook_target = nullptr;

static void __cdecl Hook_AsyncIOCallback(void* callback_data) {
    if (callback_data && g_overrides && !g_overrides->empty()) {
        auto cbd = reinterpret_cast<uintptr_t>(callback_data);
        void* resource = *reinterpret_cast<void**>(cbd + 0x08);
        uint32_t buf_index = *reinterpret_cast<uint32_t*>(cbd + 0x10);

        if (resource && buf_index == 0) {
            auto res = reinterpret_cast<uintptr_t>(resource);
            uint64_t name_hash = *reinterpret_cast<uint64_t*>(res + 0x38);
            uint64_t type_hash = *reinterpret_cast<uint64_t*>(res + 0x28);

            for (auto& ovr : *g_overrides) {
                if (ovr.applied) continue;
                if (ovr.name_hash != name_hash) continue;
                if (ovr.type_hash != 0 && ovr.type_hash != type_hash) continue;

                /* Match — replace the buffer */
                uint64_t orig_size = *reinterpret_cast<uint64_t*>(cbd + 0x20);
                *reinterpret_cast<const void**>(cbd + 0x18) = ovr.data;
                *reinterpret_cast<uint64_t*>(cbd + 0x20) = ovr.size;
                ovr.applied = true;
                g_total_hits++;

                Log(EchoVR::LogLevel::Info,
                    "[NEVR.RESOURCE] Override: %s (0x%016llx) %llu -> %llu bytes",
                    ovr.label,
                    static_cast<unsigned long long>(name_hash),
                    static_cast<unsigned long long>(orig_size),
                    static_cast<unsigned long long>(ovr.size));
                break;
            }
        }
    }

    g_orig(callback_data);
}

/* ── File loading helper ──────────────────────────────────────────── */

static void* LoadFileFromDisk(const char* path, uint64_t* out_size) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return nullptr; }

    uint64_t sz = static_cast<uint64_t>(li.QuadPart);
    void* buf = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { CloseHandle(hFile); return nullptr; }

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buf, static_cast<DWORD>(sz), &bytesRead, NULL);
    CloseHandle(hFile);

    if (!ok || bytesRead != sz) { VirtualFree(buf, 0, MEM_RELEASE); return nullptr; }

    *out_size = sz;
    return buf;
}

} // anonymous namespace

/* ── Public API ───────────────────────────────────────────────────── */

void RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                              const void* data, uint64_t size,
                              const char* label) {
    if (!g_overrides) return;
    ResourceOverride ovr = {};
    ovr.type_hash = type_hash;
    ovr.name_hash = name_hash;
    ovr.data = data;
    ovr.size = size;
    ovr.owned = false;
    ovr.label = label;
    g_overrides->push_back(ovr);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] Registered embedded override: %s (type=0x%016llx name=0x%016llx, %llu bytes)",
        label,
        static_cast<unsigned long long>(type_hash),
        static_cast<unsigned long long>(name_hash),
        static_cast<unsigned long long>(size));
}

void RegisterResourceOverrideFromFile(uint64_t type_hash, uint64_t name_hash,
                                      const char* file_path,
                                      const char* label) {
    if (!g_overrides) return;
    uint64_t size = 0;
    void* data = LoadFileFromDisk(file_path, &size);
    if (!data) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.RESOURCE] Failed to load override file: %s", file_path);
        return;
    }
    ResourceOverride ovr = {};
    ovr.type_hash = type_hash;
    ovr.name_hash = name_hash;
    ovr.data = data;
    ovr.size = size;
    ovr.owned = true;
    ovr.label = label;
    g_overrides->push_back(ovr);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] Registered file override: %s (%s, %llu bytes)",
        label, file_path, static_cast<unsigned long long>(size));
}

void InstallResourceOverride() {
    g_overrides = new std::vector<ResourceOverride>();

    /* ── Register built-in overrides ──────────────────────────────── */

    /* Splash screen texture (EchoVRCE branding) */
    RegisterResourceOverride(
        TYPE_DDS_TEXTURE, NAME_SPLASH_TEXTURE,
        kSplashDDS, kSplashDDS_len,
        "splash_texture");

    /* ── Scan _overrides/ directory for additional files ──────────── */

    char moduleDir[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, moduleDir, MAX_PATH);
    char* lastSlash = strrchr(moduleDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char overrideDir[MAX_PATH];
    snprintf(overrideDir, sizeof(overrideDir), "%s_overrides", moduleDir);

    WIN32_FIND_DATAA fd;
    char searchPattern[MAX_PATH];
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*", overrideDir);
    HANDLE hFind = FindFirstFileA(searchPattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const char* name = fd.cFileName;

            /* Parse "0xTYPE.0xNAME" or "0xNAME" filename format */
            uint64_t type_h = 0, name_h = 0;
            if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
                const char* dot = strchr(name, '.');
                if (dot && dot[1] == '0' && (dot[2] == 'x' || dot[2] == 'X')) {
                    type_h = strtoull(name + 2, nullptr, 16);
                    name_h = strtoull(dot + 3, nullptr, 16);
                } else {
                    name_h = strtoull(name + 2, nullptr, 16);
                }
            }

            if (name_h == 0) continue;

            char filePath[MAX_PATH];
            snprintf(filePath, sizeof(filePath), "%s\\%s", overrideDir, name);
            RegisterResourceOverrideFromFile(type_h, name_h, filePath, name);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    if (g_overrides->empty()) {
        /* No overrides registered — don't install the hook */
        delete g_overrides;
        g_overrides = nullptr;
        return;
    }

    /* ── Install hook ─────────────────────────────────────────────── */

    g_hook_target = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress) + 0xFA16D0);

    if (MH_CreateHook(g_hook_target, reinterpret_cast<void*>(Hook_AsyncIOCallback),
                       reinterpret_cast<void**>(&g_orig)) != MH_OK) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.RESOURCE] MH_CreateHook failed");
        return;
    }

    if (MH_EnableHook(g_hook_target) != MH_OK) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.RESOURCE] MH_EnableHook failed");
        MH_RemoveHook(g_hook_target);
        return;
    }

    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] Hook installed (%zu overrides registered)",
        g_overrides->size());
}
