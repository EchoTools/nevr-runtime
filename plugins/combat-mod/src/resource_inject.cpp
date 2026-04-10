/*
 * resource_inject.cpp — Load combat override resources from _overrides/combat/.
 *
 * Reads manifest.json to discover which resources are modified (file-based)
 * vs aliased (redirect to original combat sublevel hash at runtime).
 *
 * Modified resources are registered with gamepatches via the exported
 * NEVR_RegisterResourceOverride API. The AsyncResourceIOCallback hook
 * in resource_override.cpp intercepts their loading and swaps the buffer.
 *
 * Aliased resources use a different mechanism: the combat-mod hooks the
 * resource lookup to redirect mpl_arena_combat → mpl_lobby_b_combat so the
 * engine loads the original data from _data without any file copies.
 *
 * This avoids modifying _data archives or embedding 101MB in the DLL.
 */

#include "resource_inject.h"
#include "plugin_log.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

namespace {

/* ── Parsed manifest data ──────────────────────────────────────── */

struct AliasEntry {
    uint64_t type_hash;
    uint64_t name_hash;
    uint64_t alias_to;   /* original hash to redirect lookup to */
};

struct OverrideEntry {
    uint64_t type_hash;
    uint64_t name_hash;
    void* data;          /* VirtualAlloc'd buffer, owned */
    uint64_t size;
    char label[128];
};

static std::vector<AliasEntry> g_aliases;
static std::vector<OverrideEntry> g_overrides;

/* ── Function pointers resolved from gamepatches exports ──────── */

typedef void (*NEVR_RegisterResourceOverride_t)(
    uint64_t type_hash, uint64_t name_hash,
    const void* data, uint64_t size, const char* label);
typedef void (*NEVR_DeregisterResourceOverrides_t)(
    const void* data_start, const void* data_end);

static NEVR_RegisterResourceOverride_t g_fnRegister = nullptr;
static NEVR_DeregisterResourceOverrides_t g_fnDeregister = nullptr;

/* ── Minimal JSON string extraction (no external deps) ─────────── */

static uint64_t ParseHex(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return strtoull(s, nullptr, 16);
}

/* Extract a quoted string value for the given key from a JSON-like string.
 * NOT a real JSON parser — works for flat objects with string values. */
static bool ExtractString(const char* json, const char* key, char* out, size_t out_size) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return false;
    pos = strchr(pos + strlen(needle), ':');
    if (!pos) return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos == 'n') { out[0] = '\0'; return true; } /* null */
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
    return (*pos == 't'); /* "true" */
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
    /* Re-alloc with null terminator */
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
        combat_mod::PluginLog("Failed to find dbgcore.dll (gamepatches)");
        return false;
    }

    g_fnRegister = reinterpret_cast<NEVR_RegisterResourceOverride_t>(
        GetProcAddress(hPatches, "NEVR_RegisterResourceOverride"));
    g_fnDeregister = reinterpret_cast<NEVR_DeregisterResourceOverrides_t>(
        GetProcAddress(hPatches, "NEVR_DeregisterResourceOverrides"));

    if (!g_fnRegister) {
        combat_mod::PluginLog("NEVR_RegisterResourceOverride not found in dbgcore.dll");
        return false;
    }
    return true;
}

} // anonymous namespace

namespace combat_mod {

int LoadCombatOverrides(uintptr_t base) {
    if (!ResolveExports()) return 0;

    /* Find _overrides/combat/ next to the game binary */
    char moduleDir[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, moduleDir, MAX_PATH);
    char* lastSlash = strrchr(moduleDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char manifestPath[MAX_PATH];
    snprintf(manifestPath, sizeof(manifestPath), "%s_overrides\\combat\\manifest.json", moduleDir);

    char* json = LoadTextFile(manifestPath);
    if (!json) {
        PluginLog("No combat override manifest at %s — combat resources disabled", manifestPath);
        return 0;
    }

    PluginLog("Loading combat overrides from %s", manifestPath);

    char overrideDir[MAX_PATH];
    snprintf(overrideDir, sizeof(overrideDir), "%s_overrides\\combat", moduleDir);

    /* Parse resource entries from manifest.
     * Walk through each {...} block in the "resources" array. */
    const char* cursor = strstr(json, "\"resources\"");
    if (!cursor) { VirtualFree(json, 0, MEM_RELEASE); return 0; }

    int loaded = 0;
    const char* entry = cursor;

    while ((entry = strchr(entry, '{')) != nullptr) {
        /* Find the end of this entry */
        const char* entryEnd = strchr(entry, '}');
        if (!entryEnd) break;

        /* Extract into a null-terminated substring */
        size_t entryLen = entryEnd - entry + 1;
        char entryBuf[2048];
        if (entryLen >= sizeof(entryBuf)) { entry = entryEnd + 1; continue; }
        memcpy(entryBuf, entry, entryLen);
        entryBuf[entryLen] = '\0';

        char typeHashStr[32] = {}, nameHashStr[32] = {};
        char fileStr[256] = {}, aliasToStr[32] = {};
        char typeName[128] = {}, levelName[128] = {};

        ExtractString(entryBuf, "type_hash", typeHashStr, sizeof(typeHashStr));
        ExtractString(entryBuf, "name_hash", nameHashStr, sizeof(nameHashStr));
        ExtractString(entryBuf, "file", fileStr, sizeof(fileStr));
        ExtractString(entryBuf, "alias_to", aliasToStr, sizeof(aliasToStr));
        ExtractString(entryBuf, "type_name", typeName, sizeof(typeName));
        ExtractString(entryBuf, "level_name", levelName, sizeof(levelName));
        bool isAlias = ExtractBool(entryBuf, "alias");

        uint64_t typeHash = ParseHex(typeHashStr);
        uint64_t nameHash = ParseHex(nameHashStr);

        if (isAlias && aliasToStr[0]) {
            AliasEntry ae = {};
            ae.type_hash = typeHash;
            ae.name_hash = nameHash;
            ae.alias_to = ParseHex(aliasToStr);
            g_aliases.push_back(ae);
        } else if (fileStr[0]) {
            /* Load the override file */
            char filePath[MAX_PATH];
            snprintf(filePath, sizeof(filePath), "%s\\%s", overrideDir, fileStr);

            uint64_t size = 0;
            void* data = LoadFile(filePath, &size);
            if (data) {
                /* Register with gamepatches */
                char label[128];
                snprintf(label, sizeof(label), "%s/%s", typeName, levelName);
                g_fnRegister(typeHash, nameHash, data, size, label);

                OverrideEntry oe = {};
                oe.type_hash = typeHash;
                oe.name_hash = nameHash;
                oe.data = data;
                oe.size = size;
                snprintf(oe.label, sizeof(oe.label), "%s", label);
                g_overrides.push_back(oe);

                PluginLog("  Override: %s (%llu bytes)", label,
                          static_cast<unsigned long long>(size));
            } else {
                PluginLog("  FAILED to load: %s", filePath);
            }
        }

        loaded++;
        entry = entryEnd + 1;
    }

    VirtualFree(json, 0, MEM_RELEASE);

    PluginLog("Loaded %d resources (%zu overrides, %zu aliases)",
              loaded, g_overrides.size(), g_aliases.size());
    return loaded;
}

bool ShouldAliasResource(uint64_t type_hash, uint64_t name_hash,
                         uint64_t* alias_hash) {
    for (const auto& ae : g_aliases) {
        if (ae.type_hash == type_hash && ae.name_hash == name_hash) {
            *alias_hash = ae.alias_to;
            return true;
        }
    }
    return false;
}

void UnloadCombatOverrides() {
    /* Deregister from gamepatches first */
    if (g_fnDeregister && !g_overrides.empty()) {
        for (const auto& oe : g_overrides) {
            /* Deregister by address range (each override individually) */
            const void* start = oe.data;
            const void* end = static_cast<const uint8_t*>(oe.data) + oe.size;
            g_fnDeregister(start, end);
        }
    }

    /* Free owned data */
    for (auto& oe : g_overrides) {
        if (oe.data) {
            VirtualFree(oe.data, 0, MEM_RELEASE);
            oe.data = nullptr;
        }
    }
    g_overrides.clear();
    g_aliases.clear();

    PluginLog("Combat overrides unloaded");
}

} // namespace combat_mod
