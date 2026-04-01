/* SYNTHESIS -- custom tool code, not from binary */

// Skip EchoVR splash/legal screens by setting high version numbers
// in demoprofile.json. The game's UI flow scripts check these values
// via R15NETCLIENTPROFILEINTEXPRESSION to decide whether to show
// splash/EULA/privacy screens. With Oculus services gone, these checks
// fail every launch unless we pre-populate them.

#include "nevr_plugin_interface.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

// Profile paths to search (relative to game CWD)
static const char* PROFILE_PATHS[] = {
    "_local/echovr/users/dmo/demoprofile.json",
    "users/dmo/demoprofile.json",
    "_local/users/dmo/demoprofile.json",
    nullptr
};

// Keys to patch and their replacement values.
// Format: { "key": old_pattern, replacement }
struct VersionPatch {
    const char* key;        // JSON key name
    const char* section;    // Parent object name
    int target_value;       // Value to set
};

static const VersionPatch PATCHES[] = {
    { "eula_version",           "legal",  9999 },
    { "game_admin_version",     "legal",  9999 },
    { "points_policy_version",  "legal",  9999 },
    { "splash_screen_version",  "legal",  9999 },
    { "setup_version",          "social", 9999 },
};
static constexpr int NUM_PATCHES = sizeof(PATCHES) / sizeof(PATCHES[0]);

static std::string ReadFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0 || len > 1024 * 1024) { fclose(f); return {}; }
    fseek(f, 0, SEEK_SET);
    std::string content(static_cast<size_t>(len), '\0');
    size_t read = fread(&content[0], 1, static_cast<size_t>(len), f);
    fclose(f);
    content.resize(read);
    return content;
}

static bool WriteFile(const char* path, const std::string& content) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return written == content.size();
}

// Replace the integer value for a key like "splash_screen_version": 7
// with a new value. Handles any existing integer (positive, multi-digit).
// Returns true if a replacement was made.
static bool PatchJsonIntValue(std::string& json, const char* key, int new_value) {
    // Find "key"
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;

    // Find the colon after the key
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos++; // skip colon

    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        pos++;

    // Find the end of the integer (digits only)
    size_t num_start = pos;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
        pos++;

    if (pos == num_start) return false; // no digits found

    // Check if already at target value
    std::string current = json.substr(num_start, pos - num_start);
    char new_str[16];
    snprintf(new_str, sizeof(new_str), "%d", new_value);
    if (current == new_str) return false; // already correct

    // Replace
    json.replace(num_start, pos - num_start, new_str);
    return true;
}

static const char* FindProfile() {
    for (int i = 0; PROFILE_PATHS[i]; i++) {
        FILE* f = fopen(PROFILE_PATHS[i], "rb");
        if (f) {
            fclose(f);
            return PROFILE_PATHS[i];
        }
    }
    return nullptr;
}

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "skip_splash";
    info.description = "Skip splash/legal screens by pre-accepting version checks";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    const char* path = FindProfile();
    if (!path) {
        fprintf(stderr, "[NEVR.SPLASH] demoprofile.json not found -- splash skip unavailable\n");
        fprintf(stderr, "[NEVR.SPLASH] Searched: _local/echovr/users/dmo/, users/dmo/, _local/users/dmo/\n");
        return 0;
    }

    std::string json = ReadFile(path);
    if (json.empty()) {
        fprintf(stderr, "[NEVR.SPLASH] Could not read %s\n", path);
        return 0;
    }

    int patched = 0;
    for (int i = 0; i < NUM_PATCHES; i++) {
        if (PatchJsonIntValue(json, PATCHES[i].key, PATCHES[i].target_value)) {
            patched++;
        }
    }

    if (patched == 0) {
        fprintf(stderr, "[NEVR.SPLASH] All version checks already set -- no changes needed\n");
        return 0;
    }

    if (!WriteFile(path, json)) {
        fprintf(stderr, "[NEVR.SPLASH] Failed to write %s\n", path);
        return 0;
    }

    fprintf(stderr, "[NEVR.SPLASH] Patched %d version checks in %s -- splash screens will be skipped\n",
        patched, path);
    return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    // Nothing to clean up
}
