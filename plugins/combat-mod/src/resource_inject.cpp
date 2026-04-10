/*
 * resource_inject.cpp — Combat resource loading via aliasing + file overrides.
 *
 * Two-layer approach:
 *
 * Layer 1 (alias): Hook CArchiveLoader::LoadResource to redirect ALL
 * mpl_arena_combat (0x813EDECF5228A2BA) lookups to mpl_lobby_b_combat
 * (0xCB9977F7FC2B4526). The engine loads the original combat sublevel
 * data from _data — no file copies needed for 84 of 87 resources.
 *
 * Layer 2 (override): For the 3 modified resources (CActorData, CTransformCR,
 * CGSceneResource), register file overrides via NEVR_RegisterResourceOverride.
 * The overrides are registered with the ALIASED hash (mpl_lobby_b_combat)
 * because after the alias redirect, the engine loads the resource under that
 * hash, and the AsyncResourceIOCallback override hook matches on the CResource's
 * name_hash field which will be mpl_lobby_b_combat.
 *
 * Special case: CGSceneResource for mpl_arena_a is a modification of the
 * ARENA level (adding sublevel offset), not the combat sublevel. It's loaded
 * under its own hash and the override matches directly.
 */

#include "resource_inject.h"
#include "plugin_log.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "nevr_common.h"
#include "address_registry.h"

namespace {

/* ── Hash constants ────────────────────────────────────────────── */

constexpr uint64_t HASH_ARENA_COMBAT = 0x813EDECF5228A2BAULL;  /* mpl_arena_combat */
constexpr uint64_t HASH_LOBBY_COMBAT = 0xCB9977F7FC2B4526ULL;  /* mpl_lobby_b_combat */

/* ── Parsed manifest data ──────────────────────────────────────── */

struct OverrideEntry {
    uint64_t type_hash;
    uint64_t name_hash;       /* hash to register with (may be aliased) */
    void* data;               /* VirtualAlloc'd buffer, owned */
    uint64_t size;
    char label[128];
};

static std::vector<OverrideEntry> g_overrides;
static bool g_aliasActive = false;  /* true after manifest loads with aliases */
static uint64_t g_aliasRedirectCount = 0;

/* ── Archive loader hook ───────────────────────────────────────── */

using CArchiveLoader_LoadResource_t = int32_t(__fastcall*)(
    void*, uint64_t, int64_t, int64_t, int64_t);
static CArchiveLoader_LoadResource_t g_origLoadResource = nullptr;
static void* g_hookTarget = nullptr;

static int32_t __fastcall Hook_ArchiveLoaderLoadResource(
    void* self, uint64_t name_hash, int64_t params, int64_t callback, int64_t user_data)
{
    if (g_aliasActive && name_hash == HASH_ARENA_COMBAT) {
        g_aliasRedirectCount++;
        if (g_aliasRedirectCount <= 5 || g_aliasRedirectCount % 50 == 0) {
            combat_mod::PluginLog("Archive alias: mpl_arena_combat -> mpl_lobby_b_combat (#%llu)",
                (unsigned long long)g_aliasRedirectCount);
        }
        name_hash = HASH_LOBBY_COMBAT;
    }
    return g_origLoadResource(self, name_hash, params, callback, user_data);
}

/* ── Function pointers resolved from gamepatches exports ──────── */

using NEVR_RegisterResourceOverride_t = void(*)(
    uint64_t, uint64_t, const void*, uint64_t, const char*);
using NEVR_DeregisterResourceOverrides_t = void(*)(const void*, const void*);
using NEVR_ResetResourceOverrides_t = void(*)();

static NEVR_RegisterResourceOverride_t g_fnRegister = nullptr;
static NEVR_DeregisterResourceOverrides_t g_fnDeregister = nullptr;
static NEVR_ResetResourceOverrides_t g_fnReset = nullptr;

/* ── Minimal JSON helpers ──────────────────────────────────────── */

static uint64_t ParseHex(const char* s) {
    if (!s || !s[0]) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return strtoull(s, nullptr, 16);
}

static bool ExtractString(const char* json, const char* key, char* out, size_t out_size) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return false;
    pos = strchr(pos + strlen(needle), ':');
    if (!pos) return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos == 'n') { out[0] = '\0'; return true; }
    if (*pos != '"') return false;
    pos++;
    const char* end = strchr(pos, '"');
    if (!end) return false;
    size_t len = end - pos;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, pos, len);
    out[len] = '\0';
    return true;
}

static bool ExtractBool(const char* json, const char* key) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return false;
    pos = strchr(pos + strlen(needle), ':');
    if (!pos) return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    return (*pos == 't');
}

/* ── File I/O ──────────────────────────────────────────────────── */

static void* LoadFile(const char* path, uint64_t* out_size) {
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

static char* LoadTextFile(const char* path) {
    uint64_t size = 0;
    void* data = LoadFile(path, &size);
    if (!data) return nullptr;
    char* text = static_cast<char*>(VirtualAlloc(NULL, size + 1,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!text) { VirtualFree(data, 0, MEM_RELEASE); return nullptr; }
    memcpy(text, data, size);
    text[size] = '\0';
    VirtualFree(data, 0, MEM_RELEASE);
    return text;
}

/* ── Resolve gamepatches exports ───────────────────────────────── */

static bool ResolveExports() {
    HMODULE hPatches = GetModuleHandleA("dbgcore.dll");
    if (!hPatches) {
        combat_mod::PluginLog("dbgcore.dll not found");
        return false;
    }
    g_fnRegister = reinterpret_cast<NEVR_RegisterResourceOverride_t>(
        GetProcAddress(hPatches, "NEVR_RegisterResourceOverride"));
    g_fnDeregister = reinterpret_cast<NEVR_DeregisterResourceOverrides_t>(
        GetProcAddress(hPatches, "NEVR_DeregisterResourceOverrides"));
    g_fnReset = reinterpret_cast<NEVR_ResetResourceOverrides_t>(
        GetProcAddress(hPatches, "NEVR_ResetResourceOverrides"));

    if (!g_fnRegister) {
        combat_mod::PluginLog("NEVR_RegisterResourceOverride not found");
        return false;
    }
    return true;
}

} // anonymous namespace

namespace combat_mod {

int LoadCombatOverrides(uintptr_t base) {
    if (!ResolveExports()) return 0;

    /* Install archive loader hook for resource aliasing */
    g_hookTarget = nevr::ResolveVA(base, nevr::addresses::VA_ARCHIVE_LOADER_LOAD_RESOURCE);
    if (MH_CreateHook(g_hookTarget,
                       reinterpret_cast<void*>(Hook_ArchiveLoaderLoadResource),
                       reinterpret_cast<void**>(&g_origLoadResource)) == MH_OK) {
        MH_EnableHook(g_hookTarget);
        PluginLog("CArchiveLoader::LoadResource hooked for resource aliasing");
    } else {
        PluginLog("FAILED to hook CArchiveLoader::LoadResource");
    }

    /* Find _overrides/combat/ next to the game binary */
    char moduleDir[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, moduleDir, MAX_PATH);
    char* lastSlash = strrchr(moduleDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char manifestPath[MAX_PATH];
    snprintf(manifestPath, sizeof(manifestPath), "%s_overrides\\combat\\manifest.json", moduleDir);

    char* json = LoadTextFile(manifestPath);
    if (!json) {
        PluginLog("No combat manifest at %s", manifestPath);
        return 0;
    }

    PluginLog("Loading combat overrides from %s", manifestPath);

    char overrideDir[MAX_PATH];
    snprintf(overrideDir, sizeof(overrideDir), "%s_overrides\\combat", moduleDir);

    /* Parse global fields */
    char origHashStr[32] = {};
    ExtractString(json, "original_combat_hash", origHashStr, sizeof(origHashStr));
    uint64_t origCombatHash = ParseHex(origHashStr);

    /* Parse resource entries */
    const char* cursor = strstr(json, "\"resources\"");
    if (!cursor) { VirtualFree(json, 0, MEM_RELEASE); return 0; }

    int loaded = 0;
    int aliasCount = 0;
    int overrideCount = 0;
    const char* entry = cursor;

    while ((entry = strchr(entry, '{')) != nullptr) {
        const char* entryEnd = strchr(entry, '}');
        if (!entryEnd) break;

        size_t entryLen = entryEnd - entry + 1;
        char entryBuf[2048];
        if (entryLen >= sizeof(entryBuf)) { entry = entryEnd + 1; continue; }
        memcpy(entryBuf, entry, entryLen);
        entryBuf[entryLen] = '\0';

        char typeHashStr[32] = {}, nameHashStr[32] = {};
        char fileStr[256] = {};
        char typeName[128] = {}, levelName[128] = {};

        ExtractString(entryBuf, "type_hash", typeHashStr, sizeof(typeHashStr));
        ExtractString(entryBuf, "name_hash", nameHashStr, sizeof(nameHashStr));
        ExtractString(entryBuf, "file", fileStr, sizeof(fileStr));
        ExtractString(entryBuf, "type_name", typeName, sizeof(typeName));
        ExtractString(entryBuf, "level_name", levelName, sizeof(levelName));
        bool isAlias = ExtractBool(entryBuf, "alias");

        uint64_t typeHash = ParseHex(typeHashStr);
        uint64_t nameHash = ParseHex(nameHashStr);

        if (isAlias) {
            /* Alias — the archive loader hook handles the redirect.
             * No file needed. Just count it. */
            aliasCount++;
        } else if (fileStr[0]) {
            /* Modified resource — load from file and register override.
             * If this is a combat sublevel resource (nameHash == HASH_ARENA_COMBAT),
             * register with the ALIASED hash (mpl_lobby_b_combat) because after
             * the archive loader redirect, the engine loads under that hash. */
            char filePath[MAX_PATH];
            snprintf(filePath, sizeof(filePath), "%s\\%s", overrideDir, fileStr);

            uint64_t size = 0;
            void* data = LoadFile(filePath, &size);
            if (data) {
                /* Determine the registration hash:
                 * - Combat sublevel resources: register with origCombatHash
                 *   (the CResource will have this hash after the alias redirect)
                 * - Arena level resources: register with their own hash
                 *   (no alias redirect — they're loaded under their own name) */
                uint64_t regNameHash = nameHash;
                if (nameHash == HASH_ARENA_COMBAT && origCombatHash) {
                    regNameHash = origCombatHash;
                }

                char label[128];
                snprintf(label, sizeof(label), "combat:%s/%s", typeName, levelName);
                g_fnRegister(typeHash, regNameHash, data, size, label);

                OverrideEntry oe = {};
                oe.type_hash = typeHash;
                oe.name_hash = regNameHash;
                oe.data = data;
                oe.size = size;
                snprintf(oe.label, sizeof(oe.label), "%s", label);
                g_overrides.push_back(oe);

                overrideCount++;
                PluginLog("  Override: %s (%llu bytes, reg=0x%016llx)",
                          label, (unsigned long long)size, (unsigned long long)regNameHash);
            } else {
                PluginLog("  FAILED: %s", filePath);
            }
        }

        loaded++;
        entry = entryEnd + 1;
    }

    VirtualFree(json, 0, MEM_RELEASE);

    g_aliasActive = (aliasCount > 0);

    PluginLog("Loaded %d resources: %d overrides (%.1f MB), %d aliases",
              loaded, overrideCount,
              static_cast<double>(g_overrides.size() > 0 ?
                  g_overrides.back().size : 0) / (1024.0 * 1024.0),
              aliasCount);

    return loaded;
}

bool ShouldAliasResource(uint64_t type_hash, uint64_t name_hash,
                         uint64_t* alias_hash) {
    /* This is now handled by the CArchiveLoader::LoadResource hook directly.
     * Kept for API compatibility but not called externally. */
    if (name_hash == HASH_ARENA_COMBAT) {
        *alias_hash = HASH_LOBBY_COMBAT;
        return true;
    }
    return false;
}

void UnloadCombatOverrides() {
    /* Deregister overrides from gamepatches */
    if (g_fnDeregister) {
        for (const auto& oe : g_overrides) {
            if (oe.data) {
                const void* start = oe.data;
                const void* end = static_cast<const uint8_t*>(oe.data) + oe.size;
                g_fnDeregister(start, end);
            }
        }
    }

    /* Remove archive loader hook */
    if (g_hookTarget) {
        MH_DisableHook(g_hookTarget);
        MH_RemoveHook(g_hookTarget);
        g_hookTarget = nullptr;
    }

    /* Free owned data */
    for (auto& oe : g_overrides) {
        if (oe.data) {
            VirtualFree(oe.data, 0, MEM_RELEASE);
            oe.data = nullptr;
        }
    }
    g_overrides.clear();
    g_aliasActive = false;

    PluginLog("Combat overrides unloaded (%llu alias redirects served)",
              (unsigned long long)g_aliasRedirectCount);
}

} // namespace combat_mod
