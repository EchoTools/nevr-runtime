/* SYNTHESIS -- custom tool code, not from binary */

#include "log_filter.h"
#include "nevr_plugin_interface.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[log_filter] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

static std::string FindConfigFile() {
    /* Try YAML first, then JSONC fallback */
    const char* candidates[] = {
        "log_filter_config.yml",
        "log_filter_config.yaml",
        "log_filter_config.jsonc",
        "log_filter_config.json",
    };

#ifdef _WIN32
    char dll_path[MAX_PATH] = {};
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&FindConfigFile), &hm)) {
        GetModuleFileNameA(hm, dll_path, MAX_PATH);
        char* last_slash = std::strrchr(dll_path, '\\');
        if (!last_slash) last_slash = std::strrchr(dll_path, '/');
        if (last_slash) {
            *(last_slash + 1) = '\0';
            for (auto* name : candidates) {
                std::string path = std::string(dll_path) + name;
                FILE* f = std::fopen(path.c_str(), "r");
                if (f) { std::fclose(f); return path; }
            }
        }
    }
#endif
    for (auto* name : candidates) {
        FILE* f = std::fopen(name, "r");
        if (f) { std::fclose(f); return name; }
    }
    return {};
}

/* Default config with all the known noise patterns from the log catalog */
static LogFilterConfig MakeDefaultConfig() {
    LogFilterConfig cfg;
    cfg.min_level = 2;  /* INFO and above */
    cfg.timestamps = true;
    cfg.passthrough_to_engine = false;
    cfg.max_line_length = 500;

    /* Console */
    cfg.console_enabled = true;
    cfg.console_color = true;

    /* File: JSONL by default, rotate at 50MB, keep 5 files */
    cfg.file_enabled = true;
    cfg.file_jsonl = true;
    cfg.rotate_enabled = true;
    cfg.rotate_max_size_mb = 50;
    cfg.rotate_interval_min = 0;
    cfg.rotate_keep = 5;

    cfg.suppress_channels = {
        "[LOADING TIPS]",
        "[DIALOGUE]",
        "[DEBUGPRINT]",
    };

    cfg.suppress_patterns = {
        "Resetting player data",
        "List element 0x",
        "CGameStats::From (JSON): Stat",
        "parent set to Auto but parent in Maya",
        "No item assignment found",
        "removing feature 'rift'",
        "could not find CSubtitleCS",
        "Unable to resolve script actor",
        "Ran out of pooled objective markers",
        "SetMaxLoudnessEnabled",
        "Applying new graphics settings",
        "Entrant Data Changed",
        "LobbyOwnerDataChangedCB",
        "PlayEmoteCB for user",
        "ghosted False",
        "expecting peer connection",
        "realdiv(",
        "VoIP Muted",
        "VoIP Deafened",
        "Is Placeholder",
        "Is Ghost",
        "Is Bot",
        "Is Spectator",
        "Peer : 184467",
        "Alignment : 65535",
        "Loadout Number",
        "In VR",
        "UserID Hex",
        "Countdown UI disabled",
        "PickRandomTip:",
        "No screen stats info for game mode",
    };

    cfg.truncate_rules = {
        {"[PROFILE] Getting", 80},
        {"[PROFILE] Updating", 80},
        {"[CONFIGS] ConfigSuccessCB", 80},
    };

    cfg.valid = true;
    return cfg;
}

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "log_filter";
    info.description = "Structured log filtering with JSONL file output, rotation, and ANSI color";
    info.version_major = 2;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    Log("initializing (base=0x%llx)", static_cast<unsigned long long>(ctx->base_addr));

    /* Load config or use defaults */
    std::string config_path = FindConfigFile();
    if (config_path.empty()) {
        LogFilterConfig defaults = MakeDefaultConfig();
        Log("no config file found, using built-in defaults (%zu suppress patterns)",
            defaults.suppress_patterns.size());
        SetLogFilterConfig(defaults);
    } else {
        LogFilterConfig cfg = LoadLogFilterConfig(config_path.c_str());
        if (!cfg.valid) {
            Log("config parse failed, using built-in defaults");
            SetLogFilterConfig(MakeDefaultConfig());
        } else {
            SetLogFilterConfig(cfg);
            Log("config loaded: min_level=%u, %zu channels, %zu patterns, %zu truncate, file=%s(%s), color=%s, rotate=%s",
                cfg.min_level,
                cfg.suppress_channels.size(),
                cfg.suppress_patterns.size(),
                cfg.truncate_rules.size(),
                cfg.file_enabled ? "on" : "off",
                cfg.file_jsonl ? "jsonl" : "text",
                cfg.console_color ? "on" : "off",
                cfg.rotate_enabled ? "on" : "off");
        }
    }

    /* Install hook on CLog::PrintfImpl */
    if (!InstallLogFilterHook(ctx->base_addr)) {
        Log("failed to install hook — logging will be unfiltered");
        return -1;
    }

    Log("initialization complete — engine logging is now filtered");
    return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    Log("shutting down");
    RemoveLogFilterHook();
}
