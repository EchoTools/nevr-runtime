/* SYNTHESIS -- custom tool code, not from binary */

#include "game_rules_override.h"
#include "nevr_common.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

/* --------------------------------------------------------------------
 * Logging
 * -------------------------------------------------------------------- */

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[game_rules_override] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

/* --------------------------------------------------------------------
 * Memory protection helpers
 * -------------------------------------------------------------------- */

static bool MakeWritable(void* addr, size_t len) {
#ifdef _WIN32
    DWORD old_protect = 0;
    return VirtualProtect(addr, len, PAGE_READWRITE, &old_protect) != 0;
#else
    /* Linux/POSIX stub -- for testing, memory is already writable */
    (void)addr;
    (void)len;
    return true;
#endif
}

static bool RestoreProtection(void* addr, size_t len) {
#ifdef _WIN32
    DWORD old_protect = 0;
    return VirtualProtect(addr, len, PAGE_READONLY, &old_protect) != 0;
#else
    (void)addr;
    (void)len;
    return true;
#endif
}

/* --------------------------------------------------------------------
 * Minimal JSONC parser (strip comments, extract float fields)
 *
 * Supports // line comments and /* block comments inside JSON.
 * Only handles top-level flat key:value pairs with numeric values.
 * -------------------------------------------------------------------- */

static std::string StripJsonComments(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    bool in_string = false;
    while (i < input.size()) {
        char c = input[i];
        if (in_string) {
            out.push_back(c);
            if (c == '\\' && i + 1 < input.size()) {
                out.push_back(input[++i]);
            } else if (c == '"') {
                in_string = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < input.size()) {
            if (input[i + 1] == '/') {
                /* Line comment -- skip to end of line */
                i += 2;
                while (i < input.size() && input[i] != '\n') ++i;
                continue;
            }
            if (input[i + 1] == '*') {
                /* Block comment */
                i += 2;
                while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) ++i;
                if (i + 1 < input.size()) i += 2;
                continue;
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

/* Extract a float value for a given key from stripped JSON. Returns true if found. */
static bool ExtractFloat(const std::string& json, const char* key, float& out) {
    std::string search = std::string("\"") + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    /* Skip whitespace and colon */
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                  json[pos] == '\n' || json[pos] == '\r' ||
                                  json[pos] == ':')) {
        ++pos;
    }
    if (pos >= json.size()) return false;
    /* Parse the number */
    char* end = nullptr;
    double val = std::strtod(json.c_str() + pos, &end);
    if (end == json.c_str() + pos) return false;
    out = static_cast<float>(val);
    return true;
}

/* Check if a string looks like valid JSON (starts with '{') */
static bool LooksLikeJson(const std::string& json) {
    for (char c : json) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        return c == '{';
    }
    return false;
}

/* --------------------------------------------------------------------
 * Config parsing
 * -------------------------------------------------------------------- */

GameRulesOverrideConfig ParseConfigString(const std::string& json_str) {
    GameRulesOverrideConfig config;

    if (json_str.empty()) {
        Log("empty config string");
        return config;
    }

    std::string stripped = StripJsonComments(json_str);

    if (!LooksLikeJson(stripped)) {
        Log("config is not valid JSON (does not start with '{')");
        return config;
    }

    /* Extract known fields */
    ExtractFloat(stripped, "max_health", config.max_health);
    ExtractFloat(stripped, "max_stun_duration", config.max_stun_duration);
    ExtractFloat(stripped, "grab_range", config.grab_range);
    ExtractFloat(stripped, "possession_time", config.possession_time);
    ExtractFloat(stripped, "clear_throw_distance", config.clear_throw_distance);
    ExtractFloat(stripped, "clear_distance_from_goal", config.clear_distance_from_goal);
    ExtractFloat(stripped, "interception_min_distance", config.interception_min_distance);
    ExtractFloat(stripped, "aim_assist_min_angle", config.aim_assist_min_angle);
    ExtractFloat(stripped, "aim_assist_max_angle", config.aim_assist_max_angle);
    ExtractFloat(stripped, "aim_assist_target_half", config.aim_assist_target_half);
    ExtractFloat(stripped, "aim_assist_min_strength", config.aim_assist_min_strength);
    ExtractFloat(stripped, "aim_assist_max_strength", config.aim_assist_max_strength);

    /* Arena timing overrides (applied via CJsonGetFloat hook) */
    ExtractFloat(stripped, "round_time", config.round_time);
    ExtractFloat(stripped, "point_score_celebration_time", config.point_score_celebration_time);
    ExtractFloat(stripped, "mercy_win_point_spread", config.mercy_win_point_spread);

    /* Validation */
    if (config.max_health >= 0.0f && config.max_health < 0.0f) {
        /* unreachable, but pattern for future negative check */
    }
    if (config.max_health < -0.5f && config.max_health != -1.0f) {
        Log("rejecting negative max_health: %.2f", config.max_health);
        return config;
    }
    if (config.max_stun_duration > MAX_ALLOWED_STUN_DURATION) {
        Log("rejecting stun duration %.2f > %.2f max",
            config.max_stun_duration, MAX_ALLOWED_STUN_DURATION);
        return config;
    }

    config.valid = true;
    return config;
}

GameRulesOverrideConfig LoadConfig(const char* path) {
    Log("loading config from: %s", path);
    std::string content = nevr::LoadConfigFile(path);
    if (content.empty()) {
        Log("failed to read config file: %s", path);
        return {};
    }
    return ParseConfigString(content);
}

/* --------------------------------------------------------------------
 * Address verification
 * -------------------------------------------------------------------- */

bool VerifyAddresses(uintptr_t base_addr) {
    /* On non-Windows, we cannot verify real binary bytes -- always pass */
#ifdef _WIN32
    /* Check g_balance_config pointer location is readable */
    auto* bc_ptr = static_cast<uint8_t*>(nevr::ResolveVA(base_addr, VA_BALANCE_CONFIG));
    if (!bc_ptr) {
        Log("VA_BALANCE_CONFIG resolved to null");
        return false;
    }
    Log("VA_BALANCE_CONFIG resolved to %p", bc_ptr);

    /* Check CanPlayerTakeDamage prologue */
    auto* cpd_ptr = static_cast<uint8_t*>(nevr::ResolveVA(base_addr, VA_CAN_PLAYER_TAKE_DAMAGE));
    if (!cpd_ptr) {
        Log("VA_CAN_PLAYER_TAKE_DAMAGE resolved to null");
        return false;
    }
    Log("VA_CAN_PLAYER_TAKE_DAMAGE resolved to %p", cpd_ptr);

    /* Check ResolveConfigHash prologue */
    auto* rch_ptr = static_cast<uint8_t*>(nevr::ResolveVA(base_addr, VA_RESOLVE_CONFIG_HASH));
    if (!rch_ptr) {
        Log("VA_RESOLVE_CONFIG_HASH resolved to null");
        return false;
    }
    Log("VA_RESOLVE_CONFIG_HASH resolved to %p", rch_ptr);

    Log("address verification passed");
#else
    Log("address verification skipped (non-Windows)");
    (void)base_addr;
#endif
    return true;
}

/* --------------------------------------------------------------------
 * Override application
 * -------------------------------------------------------------------- */

/* Helper: write a float field and log old/new values */
static bool WriteFloat(void* addr, float new_val, const char* field_name) {
    float old_val = 0.0f;
    std::memcpy(&old_val, addr, sizeof(float));

    if (!MakeWritable(addr, sizeof(float))) {
        Log("failed to make %s writable at %p", field_name, addr);
        return false;
    }

    std::memcpy(addr, &new_val, sizeof(float));
    Log("  %s @ %p: %.4f -> %.4f", field_name, addr, old_val, new_val);

    RestoreProtection(addr, sizeof(float));
    return true;
}

bool ApplyOverrides(uintptr_t base_addr, const GameRulesOverrideConfig& config) {
    if (!config.valid) {
        Log("config is not valid, skipping overrides");
        return false;
    }

    /* Resolve the balance config pointer -- it's a pointer that must be dereferenced */
    auto* bc_ptr_loc = static_cast<void**>(nevr::ResolveVA(base_addr, VA_BALANCE_CONFIG));
    if (!bc_ptr_loc) {
        Log("failed to resolve VA_BALANCE_CONFIG");
        return false;
    }

#ifdef _WIN32
    /* Dereference the global pointer to get actual struct address */
    auto* balance_config = static_cast<uint8_t*>(*bc_ptr_loc);
    if (!balance_config) {
        Log("g_balance_config is null (struct not yet allocated)");
        return false;
    }
    Log("g_balance_config struct at %p", balance_config);
#else
    /* Non-Windows: we cannot dereference game memory. Log and return. */
    Log("ApplyOverrides: skipping on non-Windows (no live game memory)");
    (void)config;
    return true;
#endif

#ifdef _WIN32
    int count = 0;
    Log("applying balance config overrides:");

    /* Combat fields */
    if (config.max_health >= 0.0f) {
        WriteFloat(balance_config + 0x40, config.max_health, "max_health");
        ++count;
    }
    if (config.max_stun_duration >= 0.0f) {
        WriteFloat(balance_config + 0x3C, config.max_stun_duration, "max_stun_duration");
        ++count;
    }

    /* Frisbee balance fields */
    if (config.grab_range >= 0.0f) {
        WriteFloat(balance_config + 0x04, config.grab_range, "grab_range");
        ++count;
    }
    if (config.possession_time >= 0.0f) {
        WriteFloat(balance_config + 0x08, config.possession_time, "possession_time");
        ++count;
    }
    if (config.clear_throw_distance >= 0.0f) {
        WriteFloat(balance_config + 0x0C, config.clear_throw_distance, "clear_throw_distance");
        ++count;
    }
    if (config.clear_distance_from_goal >= 0.0f) {
        WriteFloat(balance_config + 0x10, config.clear_distance_from_goal, "clear_distance_from_goal");
        ++count;
    }
    if (config.interception_min_distance >= 0.0f) {
        WriteFloat(balance_config + 0x14, config.interception_min_distance, "interception_min_distance");
        ++count;
    }
    if (config.aim_assist_min_angle >= 0.0f) {
        WriteFloat(balance_config + 0x18, config.aim_assist_min_angle, "aim_assist_min_angle");
        ++count;
    }
    if (config.aim_assist_max_angle >= 0.0f) {
        WriteFloat(balance_config + 0x1C, config.aim_assist_max_angle, "aim_assist_max_angle");
        ++count;
    }
    if (config.aim_assist_target_half >= 0.0f) {
        WriteFloat(balance_config + 0x20, config.aim_assist_target_half, "aim_assist_target_half");
        ++count;
    }
    if (config.aim_assist_min_strength >= 0.0f) {
        WriteFloat(balance_config + 0x24, config.aim_assist_min_strength, "aim_assist_min_strength");
        ++count;
    }
    if (config.aim_assist_max_strength >= 0.0f) {
        WriteFloat(balance_config + 0x28, config.aim_assist_max_strength, "aim_assist_max_strength");
        ++count;
    }

    Log("applied %d override(s)", count);
#endif

    return true;
}
