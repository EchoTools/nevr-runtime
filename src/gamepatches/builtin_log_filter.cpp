/* SYNTHESIS -- custom tool code, not from binary */
/*
 * Built-in log filter — adapted from plugins/log-filter for direct compilation
 * into gamepatches.dll. Hooks CLog::PrintfImpl to provide structured, filtered
 * log output with suppression, truncation, ANSI color, JSONL file output, and
 * file rotation.
 *
 * Differences from the plugin version:
 *   - No NvrPlugin* exports (built-in, not dynamically loaded)
 *   - No MH_Initialize/MH_Uninitialize (gamepatches core owns MinHook lifecycle)
 *   - No MH_DisableHook(MH_ALL_HOOKS) (would kill all hooks process-wide)
 *   - Config discovery searches relative to game directory, not plugins/ subdir
 */

#include "builtin_log_filter.h"
#include "common/logging.h"

#include <MinHook.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "address_registry.h"
#include "nevr_common.h"
#include "yaml_config.h"

/* ------------------------------------------------------------------ */
/* Log levels (from CLog.h ELogLevel)                                  */
/* ------------------------------------------------------------------ */

static constexpr uint32_t LOG_LEVEL_DEBUG   = 1;
static constexpr uint32_t LOG_LEVEL_INFO    = 2;
static constexpr uint32_t LOG_LEVEL_WARNING = 4;
static constexpr uint32_t LOG_LEVEL_ERROR   = 8;
static constexpr uint32_t LOG_LEVEL_RAW     = 1000;

/* ------------------------------------------------------------------ */
/* Config types                                                        */
/* ------------------------------------------------------------------ */

struct TruncateRule {
    std::string prefix;
    int max_length = 0;
};

struct LogFilterConfig {
    uint32_t    min_level = LOG_LEVEL_INFO;
    std::vector<std::string> suppress_channels;
    std::vector<std::string> suppress_patterns;
    std::vector<TruncateRule> truncate_rules;
    int         max_line_length = 500;
    bool        timestamps = true;
    bool        passthrough_to_engine = false;

    bool        console_enabled = true;
    bool        console_color = true;

    bool        file_enabled = false;
    std::string file_dir;
    bool        file_jsonl = true;

    bool        rotate_enabled = false;
    int         rotate_max_size_mb = 50;
    int         rotate_interval_min = 0;
    int         rotate_keep = 5;

    bool        valid = false;
};

/* ------------------------------------------------------------------ */
/* Bootstrap logging (before hook is installed)                         */
/* ------------------------------------------------------------------ */

static void BlfLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[builtin_log_filter] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static LogFilterConfig g_config;
static uintptr_t g_base_addr = 0;
static std::atomic<uint64_t> g_suppressed_count{0};
static std::atomic<uint64_t> g_emitted_count{0};
/* N89 follow-up: count lines the GAME produced, separately from our own
 * [NEVR.*] output. N79's emitted/suppressed totals could not catch N89 because
 * our own lines kept them nonzero while the filter captured zero game output for
 * an entire run. "Is it emitting?" was the wrong question; "is it seeing the
 * thing it exists to filter?" is the right one. */
static std::atomic<uint64_t> g_game_line_count{0};

/* File output state */
static FILE* g_log_file = nullptr;
static std::mutex g_file_mutex;
static std::string g_log_file_path;
static uint64_t g_file_bytes_written = 0;
static uint64_t g_file_open_time = 0;
static std::string g_log_file_prefix;
static std::string g_log_dir;

/* va_list must be a simple pointer on x64 for our int64_t* cast to work */
static_assert(sizeof(va_list) == sizeof(void*),
              "va_list must be a pointer type for int64_t*-to-va_list cast");

/* ------------------------------------------------------------------ */
/* Platform helpers                                                    */
/* ------------------------------------------------------------------ */

static uint64_t GetEpochSeconds() {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t / 10000000ULL) - 11644473600ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec);
#endif
}

/* ------------------------------------------------------------------ */
/* Config parsing                                                      */
/* ------------------------------------------------------------------ */

static std::string StripComments(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool in_string = false;
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == '"' && (i == 0 || input[i - 1] != '\\')) {
            in_string = !in_string;
            out += input[i++];
        } else if (!in_string && i + 1 < input.size() && input[i] == '/' && input[i + 1] == '/') {
            while (i < input.size() && input[i] != '\n') ++i;
        } else if (!in_string && i + 1 < input.size() && input[i] == '/' && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) ++i;
            if (i + 1 < input.size()) i += 2;
        } else {
            out += input[i++];
        }
    }
    return out;
}

static LogFilterConfig ParseJsonConfig(const std::string& json_str) {
    std::string clean = StripComments(json_str);
    LogFilterConfig cfg;

    try {
        auto j = nlohmann::json::parse(clean);

        cfg.min_level = j.value("min_level", cfg.min_level);
        cfg.max_line_length = j.value("max_line_length", cfg.max_line_length);
        cfg.timestamps = j.value("timestamps", cfg.timestamps);
        cfg.passthrough_to_engine = j.value("passthrough_to_engine", cfg.passthrough_to_engine);

        cfg.console_enabled = j.value("console_enabled", cfg.console_enabled);
        cfg.console_color = j.value("console_color", cfg.console_color);

        cfg.file_enabled = j.value("file_enabled", cfg.file_enabled);
        cfg.file_dir = j.value("file_dir", cfg.file_dir);
        cfg.file_jsonl = j.value("file_jsonl", cfg.file_jsonl);

        cfg.rotate_enabled = j.value("rotate_enabled", cfg.rotate_enabled);
        cfg.rotate_max_size_mb = j.value("rotate_max_size_mb", cfg.rotate_max_size_mb);
        cfg.rotate_interval_min = j.value("rotate_interval_min", cfg.rotate_interval_min);
        cfg.rotate_keep = j.value("rotate_keep", cfg.rotate_keep);

        if (j.contains("suppress_channels")) {
            for (const auto& s : j["suppress_channels"])
                cfg.suppress_channels.push_back(s.get<std::string>());
        }
        if (j.contains("suppress_patterns")) {
            for (const auto& s : j["suppress_patterns"])
                cfg.suppress_patterns.push_back(s.get<std::string>());
        }

        if (j.contains("truncate_patterns")) {
            for (const auto& item : j["truncate_patterns"]) {
                TruncateRule rule;
                rule.prefix = item.value("prefix", "");
                rule.max_length = item.value("max_length", 0);
                if (!rule.prefix.empty() && rule.max_length > 0)
                    cfg.truncate_rules.push_back(rule);
            }
        }
    } catch (...) {
        return cfg;
    }

    if (cfg.max_line_length < 0) cfg.max_line_length = 0;
    cfg.valid = true;
    return cfg;
}

static LogFilterConfig ParseYamlLogFilterConfig(const std::string& yaml_str) {
    YamlConfig y = ParseYamlConfig(yaml_str);
    LogFilterConfig cfg;

    if (y.count("min_level"))          cfg.min_level = y["min_level"].as_uint32(LOG_LEVEL_INFO);
    if (y.count("max_line_length"))    cfg.max_line_length = y["max_line_length"].as_int(500);
    if (y.count("timestamps"))         cfg.timestamps = y["timestamps"].as_bool(true);
    if (y.count("passthrough_to_engine")) cfg.passthrough_to_engine = y["passthrough_to_engine"].as_bool(false);

    if (y.count("console_enabled"))    cfg.console_enabled = y["console_enabled"].as_bool(true);
    if (y.count("console_color"))      cfg.console_color = y["console_color"].as_bool(true);

    if (y.count("file_enabled"))       cfg.file_enabled = y["file_enabled"].as_bool(false);
    if (y.count("file_dir"))           cfg.file_dir = y["file_dir"].str;
    if (y.count("file_jsonl"))         cfg.file_jsonl = y["file_jsonl"].as_bool(true);

    if (y.count("rotate_enabled"))     cfg.rotate_enabled = y["rotate_enabled"].as_bool(false);
    if (y.count("rotate_max_size_mb")) cfg.rotate_max_size_mb = y["rotate_max_size_mb"].as_int(50);
    if (y.count("rotate_interval_min")) cfg.rotate_interval_min = y["rotate_interval_min"].as_int(0);
    if (y.count("rotate_keep"))        cfg.rotate_keep = y["rotate_keep"].as_int(5);

    if (y.count("suppress_channels"))  cfg.suppress_channels = y["suppress_channels"].list;
    if (y.count("suppress_patterns"))  cfg.suppress_patterns = y["suppress_patterns"].list;

    if (y.count("truncate_patterns") && y["truncate_patterns"].is_object_list) {
        for (auto& obj : y["truncate_patterns"].object_list) {
            TruncateRule rule;
            if (obj.count("prefix")) rule.prefix = obj["prefix"];
            if (obj.count("max_length")) rule.max_length = std::atoi(obj["max_length"].c_str());
            if (!rule.prefix.empty() && rule.max_length > 0) {
                cfg.truncate_rules.push_back(rule);
            }
        }
    }

    if (cfg.max_line_length < 0) cfg.max_line_length = 0;
    cfg.valid = true;
    return cfg;
}

static LogFilterConfig LoadConfigFromFile(const char* path) {
    std::string content = nevr::LoadConfigFile(path);
    if (content.empty()) {
        BlfLog("failed to read config file: %s", path);
        return {};
    }
    std::string p(path);
    if ((p.size() >= 4 && p.substr(p.size() - 4) == ".yml") ||
        (p.size() >= 5 && p.substr(p.size() - 5) == ".yaml")) {
        return ParseYamlLogFilterConfig(content);
    }
    return ParseJsonConfig(content);
}

/* ------------------------------------------------------------------ */
/* Config discovery — searches relative to game directory               */
/* ------------------------------------------------------------------ */

static std::string FindConfigFile() {
    const char* candidates[] = {
        "log_filter_config.yml",
        "log_filter_config.yaml",
        "log_filter_config.jsonc",
        "log_filter_config.json",
    };

#ifdef _WIN32
    /* Search relative to the game executable */
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    char* last_slash = std::strrchr(exe_path, '\\');
    if (!last_slash) last_slash = std::strrchr(exe_path, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0';
        for (auto* name : candidates) {
            std::string path = std::string(exe_path) + name;
            FILE* f = std::fopen(path.c_str(), "r");
            if (f) { std::fclose(f); return path; }
        }
    }
#endif
    /* Fallback: current working directory */
    for (auto* name : candidates) {
        FILE* f = std::fopen(name, "r");
        if (f) { std::fclose(f); return name; }
    }
    return {};
}

/* ------------------------------------------------------------------ */
/* Default config with known noise patterns                            */
/* ------------------------------------------------------------------ */

static LogFilterConfig MakeDefaultConfig() {
    LogFilterConfig cfg;
    cfg.min_level = LOG_LEVEL_INFO;
    cfg.timestamps = true;
    cfg.passthrough_to_engine = false;
    cfg.max_line_length = 500;

    cfg.console_enabled = true;
    cfg.console_color = true;

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
        // -- original patterns --------------------------------------------------
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

        // -- N18 audit additions (docs/standards/logging.md Rule 4) ---------------------------
        //
        // DATA: sampled echovr-server-32-2026-06-28T21-47-43.698.jsonl
        //       (299,187 lines, ~50 MB rotated). 97.5% of sustained lines were
        //       "[NEVR.PATCH] ExitProcess(3) suppressed in server mode (call #N)".
        //       Game-native startup lines (graphics settings, engine init,
        //       battle-pass/store cosmetics, allocator stats) accounted for the
        //       remainder.

        // REMOVED 2026-07-26 (N77/N78): "ExitProcess(".
        // It was the #1 noise source (97.5% of sustained volume) — but it is a
        // NEVR-emitted line, and matching it by substring also deleted
        // "[NEVR.PATCH] ExitProcess(%u) called" (crash_recovery.cpp:140), the report
        // of a REAL, allowed process exit. A rule that cannot tell a suppressed exit
        // from a real one is not a noise rule. NEVR lines are now exempt from these
        // patterns (ShouldSuppress/N77) and the volume is handled by rate-limited
        // summarisation (RateCheck/N78), which collapses the run into a counted line
        // instead of erasing the event.

        // Graphics settings block — irrelevant for headless server, repeated at
        // startup, no operational value in any mode.
        "Graphics quality:",
        "MSAA mode:",
        "TAA:",
        "Multi-Res:",
        "Adaptive Res:",
        "Resolution Scale:",
        "Fullscreen:",
        "[SETTINGS TRANSLATOR] processing setting",

        // Engine internal initialization chatter — bounded at startup but pure
        // noise with no diagnostic value for operators.
        "Loading global archives",
        "Loading game archives",
        "Loading archive 0x",
        // REMOVED 2026-07-26 (N77): "Finished initializing engine".
        // This string is the witness quoted in the N7, N8 and N10 close records as
        // the proof that headless boot advanced past each render gate. It fires ONCE
        // per boot, so suppressing it saved nothing measurable and destroyed the
        // evidence the next headless regression would be diagnosed with.
        "Initializing enumerate thread",
        "Forking enumerate thread",

        // Oculus / RAD provider init detail — engine lifecycle that no operator
        // reads.
        "Creating OVR provider",
        "Creating RAD provider",
        "Using RAD provider for mic input",

        // Battle pass and store cosmetic state — 186650 days remaining means
        // "forever"; this is commerce config, not ops.
        "CR15NetBattlePass",
        "CR15NetStore",

        // Memory allocator stats per level load — verbose internal detail.
        "[LEVELLOAD] Allocator used:",

        // -- dbgcore.dll symbol handler failure (harmless under Wine) ------------
        // The game's symbol handler tries to load Windows debug-help DLLs
        // (dbgcore.dll, dbghelp.dll, symsrv.dll) at Error level. Under Wine,
        // dbgcore.dll is not available — the failure is cosmetic since the
        // symbol handler is a debug facility, not a functional requirement.
        // The same "Failed to initialize symbol handler" prefix fires for all
        // three DLLs; we suppress only dbgcore (the one Wine definitively lacks).
        "couldn't load dbgcore.dll",
    };

    cfg.truncate_rules = {
        {"[PROFILE] Getting", 80},
        {"[PROFILE] Updating", 80},
        {"[CONFIGS] ConfigSuccessCB", 80},
    };

    cfg.valid = true;
    return cfg;
}

/* ------------------------------------------------------------------ */
/* Timestamp helpers                                                   */
/* ------------------------------------------------------------------ */

static void FormatTimestamp(char* buf, size_t buf_size) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
#endif
}

static void FormatTimestampFilename(char* buf, size_t buf_size) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(buf, buf_size, "%04d-%02d-%02dT%02d-%02d-%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(buf, buf_size, "%04d-%02d-%02dT%02d-%02d-%02d.%03ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
#endif
}

/* ------------------------------------------------------------------ */
/* File output                                                         */
/* ------------------------------------------------------------------ */

static std::string BuildLogFilePath() {
    char ts[32];
    FormatTimestampFilename(ts, sizeof(ts));
    const char* ext = g_config.file_jsonl ? ".jsonl" : ".log";
    std::string path = g_log_dir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += g_log_file_prefix + "-" + ts + ext;
    return path;
}

static void OpenLogFile() {
    g_log_file_path = BuildLogFilePath();
    g_log_file = std::fopen(g_log_file_path.c_str(), "ab");
    if (!g_log_file) {
        BlfLog("failed to open log file: %s", g_log_file_path.c_str());
        return;
    }
    g_file_bytes_written = 0;
    g_file_open_time = GetEpochSeconds();
    BlfLog("logging to: %s", g_log_file_path.c_str());
}

static void CloseLogFile() {
    if (g_log_file) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

static void PruneOldLogs() {
    if (g_config.rotate_keep <= 0) return;

    std::vector<std::string> files;
    const char* ext = g_config.file_jsonl ? ".jsonl" : ".log";

#ifdef _WIN32
    std::string pattern = g_log_dir;
    if (!pattern.empty() && pattern.back() != '\\') pattern += '\\';
    pattern += g_log_file_prefix + "-*" + ext;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string full = g_log_dir;
            if (!full.empty() && full.back() != '\\') full += '\\';
            full += fd.cFileName;
            files.push_back(full);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* dir = opendir(g_log_dir.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name.find(g_log_file_prefix) == 0 &&
                name.size() > std::strlen(ext) &&
                name.compare(name.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
                std::string full = g_log_dir + "/" + name;
                files.push_back(full);
            }
        }
        closedir(dir);
    }
#endif

    std::sort(files.begin(), files.end());

    int to_delete = static_cast<int>(files.size()) - g_config.rotate_keep;
    for (int i = 0; i < to_delete; i++) {
        std::remove(files[i].c_str());
    }
}

static bool ShouldRotate() {
    if (!g_config.rotate_enabled || !g_log_file) return false;

    if (g_config.rotate_max_size_mb > 0) {
        uint64_t limit = static_cast<uint64_t>(g_config.rotate_max_size_mb) * 1024 * 1024;
        if (g_file_bytes_written >= limit) return true;
    }

    if (g_config.rotate_interval_min > 0) {
        uint64_t elapsed = GetEpochSeconds() - g_file_open_time;
        if (elapsed >= static_cast<uint64_t>(g_config.rotate_interval_min) * 60) return true;
    }

    return false;
}

static void RotateIfNeeded() {
    if (!ShouldRotate()) return;
    CloseLogFile();
    PruneOldLogs();
    OpenLogFile();
}

static std::string GetDefaultLogDir() {
#ifdef _WIN32
    // %LOCALAPPDATA%\EchoVR\logs — proper var directory, not application dir
    char buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string dir(buf, len);
        dir += "\\EchoVR";
        CreateDirectoryA(dir.c_str(), nullptr);
        dir += "\\logs";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }
    // Fallback: module directory (shouldn't happen but be safe)
    char mod_path[MAX_PATH];
    GetModuleFileNameA(nullptr, mod_path, MAX_PATH);
    std::string dir(mod_path);
    auto slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    dir += "\\logs";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
#else
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.local/share/echovr/logs";
    mkdir((std::string(home) + "/.local").c_str(), 0755);
    mkdir((std::string(home) + "/.local/share").c_str(), 0755);
    mkdir((std::string(home) + "/.local/share/echovr").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    return dir;
#endif
}

static void InitFileLogging() {
    if (!g_config.file_enabled) return;

    g_log_file_prefix = "nevr";

    if (g_config.file_dir.empty()) {
        g_log_dir = GetDefaultLogDir();
    } else {
        g_log_dir = g_config.file_dir;
#ifdef _WIN32
        CreateDirectoryA(g_log_dir.c_str(), nullptr);
#else
        mkdir(g_log_dir.c_str(), 0755);
#endif
    }

    OpenLogFile();
}

static void ShutdownFileLogging() {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    CloseLogFile();
}

/* ------------------------------------------------------------------ */
/* JSON escaping for JSONL output                                      */
/* ------------------------------------------------------------------ */

static void JsonEscapeAppend(std::string& out, const char* s, int len) {
    out.reserve(out.size() + len + 16);
    for (int i = 0; i < len; i++) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned char>(c));
                    out += esc;
                } else {
                    out += c;
                }
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Filtering logic                                                     */
/* ------------------------------------------------------------------ */

static const char* LevelStr(uint32_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG:   return "debug";
        case LOG_LEVEL_INFO:    return "info";
        case LOG_LEVEL_WARNING: return "warn";
        case LOG_LEVEL_ERROR:   return "error";
        default:                return "info";
    }
}

/* N77: a line NEVR emitted, identified by its subsystem tag. docs/standards/logging.md requires
 * every NEVR line to begin with `[NEVR.` — that prefix is the discriminator. */
static bool IsNevrLine(const char* message) {
    return std::strncmp(message, "[NEVR.", 6) == 0;
}

/* N77 — suppression is scoped to the emitter it targets.
 *
 * `suppress_patterns` exist to delete echovr-native noise. They are matched with
 * strstr against the FULLY FORMATTED message, so before this guard they also
 * deleted NEVR's own structured output whenever a substring happened to collide.
 * That was not hypothetical: `"Finished initializing engine"` is the witness string
 * quoted in the N7/N8/N10 close records as proof headless boot advanced, and
 * `"ExitProcess("` masked `[NEVR.PATCH] ExitProcess(%u) called` — the report of a
 * REAL process exit — alongside the noisy suppression line it was aimed at.
 *
 * A NEVR line is therefore exempt from game-noise patterns. NEVR lines that are
 * genuinely too frequent are handled by rate-limited summarisation (N78), which
 * collapses them into a counted line instead of deleting them: "this event is
 * frequent" and "this event does not matter" are different claims.
 *
 * min_level and suppress_channels still apply to NEVR lines — those are level and
 * emitter decisions, not content guesses. */
static bool ShouldSuppress(const char* message, uint32_t level) {
    if (!g_config.valid) return false;

    if (level < g_config.min_level && level != LOG_LEVEL_RAW) return true;

    for (const auto& ch : g_config.suppress_channels) {
        if (std::strncmp(message, ch.c_str(), ch.size()) == 0) return true;
    }

    if (IsNevrLine(message)) return false;  // N77

    for (const auto& pat : g_config.suppress_patterns) {
        if (std::strstr(message, pat.c_str()) != nullptr) return true;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* N78 — rate-limited summarisation                                    */
/* ------------------------------------------------------------------ */
/*
 * docs/standards/logging.md Rule 4 requires that a line repeating more than once per second be
 * collapsed into "X repeated N times in the last T seconds". No such mechanism
 * existed: ShouldSuppress returned bool and the caller returned, so the only two
 * expressible actions were pass and delete. That forced every high-frequency line
 * into a binary choice between flooding the log and erasing the event.
 *
 * Identity is a digit-insensitive hash: "ExitProcess(3) ... (call #41)" and
 * "ExitProcess(3) ... (call #42)" must collapse together, so runs of digits are
 * folded to a single sentinel before hashing. Fixed-size table, no heap in the hot
 * path, its own mutex (never g_file_mutex — that is the lock the crash path used to
 * deadlock on, see N70).
 */

static constexpr uint32_t kRateWindowSec = 5;
static constexpr uint32_t kRateThreshold = 3;   /* allow this many per window, then collapse */
static constexpr size_t   kRateTableSize = 128;

struct RateEntry {
    uint64_t hash = 0;
    uint64_t window_start = 0;
    uint32_t seen = 0;        /* total occurrences this window */
    uint32_t emitted = 0;     /* how many we let through this window */
    char     sample[128] = {};
};

static RateEntry g_rate[kRateTableSize];
static std::mutex g_rate_mutex;

/* FNV-1a over the message with digit runs folded to a single 0x01 sentinel, so
 * lines differing only in counters/IDs share an identity. */
static uint64_t NormalizedHash(const char* s, int len) {
    uint64_t h = 1469598103934665603ULL;
    bool in_digits = false;
    for (int i = 0; i < len; i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= '0' && c <= '9') {
            if (in_digits) continue;
            in_digits = true;
            c = 0x01;
        } else {
            in_digits = false;
        }
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

enum class RateVerdict { Emit, Collapse };

/* Decides whether this line is emitted or folded into a counter. When a window
 * closes with folded lines, `out_summary` is filled with the summary to emit. */
static RateVerdict RateCheck(const char* msg, int len, char* out_summary, size_t summary_cap) {
    out_summary[0] = '\0';
    const uint64_t h = NormalizedHash(msg, len);
    const uint64_t now = GetEpochSeconds();

    std::lock_guard<std::mutex> lock(g_rate_mutex);
    RateEntry& e = g_rate[h % kRateTableSize];

    /* Different line hashing to this slot, or the window has expired: close out the
     * previous occupant first (emitting its summary if it collapsed anything). */
    if (e.hash != h || now - e.window_start >= kRateWindowSec) {
        if (e.hash != 0 && e.seen > e.emitted) {
            snprintf(out_summary, summary_cap,
                     "[NEVR.LOGFILTER] repeated count=%u window_s=%llu msg=\"%s\"",
                     e.seen - e.emitted,
                     static_cast<unsigned long long>(now - e.window_start),
                     e.sample);
        }
        e.hash = h;
        e.window_start = now;
        e.seen = 0;
        e.emitted = 0;
        int copy = len < static_cast<int>(sizeof(e.sample)) - 1 ? len : static_cast<int>(sizeof(e.sample)) - 1;
        memcpy(e.sample, msg, static_cast<size_t>(copy));
        e.sample[copy] = '\0';
    }

    e.seen++;
    if (e.emitted < kRateThreshold) {
        e.emitted++;
        return RateVerdict::Emit;
    }
    return RateVerdict::Collapse;
}

static int ApplyTruncation(const char* message, int len) {
    for (const auto& rule : g_config.truncate_rules) {
        if (std::strncmp(message, rule.prefix.c_str(), rule.prefix.size()) == 0) {
            if (len > rule.max_length) return rule.max_length;
        }
    }
    if (g_config.max_line_length > 0 && len > g_config.max_line_length) {
        return g_config.max_line_length;
    }
    return len;
}

/* ------------------------------------------------------------------ */
/* ANSI color codes                                                    */
/* ------------------------------------------------------------------ */

static const char* ColorForLevel(uint32_t level) {
    if (!g_config.console_color) return "";
    switch (level) {
        case LOG_LEVEL_DEBUG:   return "\033[36m";
        case LOG_LEVEL_INFO:    return "\033[0m";
        case LOG_LEVEL_WARNING: return "\033[33m";
        case LOG_LEVEL_ERROR:   return "\033[31m";
        default:                return "\033[0m";
    }
}

static const char* ColorReset() {
    return g_config.console_color ? "\033[0m" : "";
}

static const char* ColorDim() {
    return g_config.console_color ? "\033[90m" : "";
}

/* ------------------------------------------------------------------ */
/* Emit                                                                */
/* ------------------------------------------------------------------ */

static void EmitLine(uint32_t level, const char* message, int len) {
    char ts[32] = {};
    if (g_config.timestamps) {
        FormatTimestamp(ts, sizeof(ts));
    }

    const char* lvl = LevelStr(level);

    /* Console output */
    if (g_config.console_enabled) {
        FILE* out = stderr;
        if (g_config.timestamps) {
            std::fprintf(out, "%s%s%s %s%s%s %.*s%s\n",
                         ColorDim(), ts, ColorReset(),
                         ColorForLevel(level), lvl, ColorReset(),
                         len, message, ColorReset());
        } else {
            std::fprintf(out, "%s%s%s %.*s%s\n",
                         ColorForLevel(level), lvl, ColorReset(),
                         len, message, ColorReset());
        }
        std::fflush(out);
    }

    /* File output */
    if (g_config.file_enabled && g_log_file) {
        std::lock_guard<std::mutex> lock(g_file_mutex);

        if (g_config.file_jsonl) {
            std::string line = "{\"ts\":\"";
            line += ts;
            line += "\",\"run\":\"";   /* N80 — correlates with nevr-boot.jsonl */
            line += GetRunId();
            line += "\",\"level\":\"";
            line += lvl;
            line += "\",\"msg\":\"";
            JsonEscapeAppend(line, message, len);
            line += "\"}\n";

            size_t written = std::fwrite(line.data(), 1, line.size(), g_log_file);
            g_file_bytes_written += written;
        } else {
            int n;
            if (g_config.timestamps) {
                n = std::fprintf(g_log_file, "%s %s %.*s\n", ts, lvl, len, message);
            } else {
                n = std::fprintf(g_log_file, "%s %.*s\n", lvl, len, message);
            }
            if (n > 0) g_file_bytes_written += n;
        }

        std::fflush(g_log_file);
        RotateIfNeeded();
    }

    g_emitted_count++;
}

/* ------------------------------------------------------------------ */
/* N79 — filter health, emitted while the process is alive             */
/* ------------------------------------------------------------------ */
/*
 * g_emitted_count / g_suppressed_count were reported only from
 * BuiltinLogFilter::Shutdown(), which runs only from DllMain's DLL_PROCESS_DETACH
 * with lpReserved == NULL (dllmain.cpp:102) — i.e. dynamic unload only. Every real
 * server exit goes ForceFatalExit -> TerminateProcess (crash_recovery.cpp:463),
 * which performs no DLL detach at all. So the one metric that says whether the
 * filter is working was structurally unreachable in production.
 *
 * N18's invariant is "validated by a measured suppression ratio". A ratio only
 * measurable at a shutdown that never happens is not instrumentation. Emit it on a
 * cadence from a path the running process actually takes.
 */

/* 120s, not 300s. Two reasons, both learned by falsifying this check: the delta
 * test needs TWO reports before it can fire, so 300s left an inert filter
 * unreported for 6 minutes; and verifying the check needed a 7-minute server
 * run, which is how a diagnostic ends up never re-verified. 120s gives detection
 * in ~3 min at 30 lines/hour. */
static constexpr uint32_t kHealthIntervalSec = 120;
/* First report comes early: a filter that is inert from the start should be
 * visible within a minute, not after five. */
static constexpr uint32_t kFirstHealthSec = 60;
static std::atomic<bool> g_first_health_done{false};
static std::atomic<uint64_t> g_last_health_report{0};

static void EmitLine(uint32_t level, const char* message, int len);  /* fwd */

/* N89 follow-up, second defect: this was called ONLY from inside hook_PrintfImpl.
 * When another module takes the CLog hook, our handler stops running — so the
 * health check stopped running too, and could never report the one failure it
 * exists to detect. Falsified end-to-end: with the superseded plugin allowed to
 * load, NO health line appeared at all.
 *
 * It is now also driven from the N86 per-frame tick, a site whose liveness is
 * independently proven. Same lesson as N86 and N88: a monitor must not depend on
 * the thing it monitors. */
static void MaybeEmitHealth() {
    const uint64_t now = GetEpochSeconds();
    uint64_t last = g_last_health_report.load(std::memory_order_relaxed);
    if (last == 0) {
        g_last_health_report.store(now, std::memory_order_relaxed);
        return;
    }
    const uint32_t due = g_first_health_done.load(std::memory_order_relaxed)
                             ? kHealthIntervalSec : kFirstHealthSec;
    if (now - last < due) return;
    /* Only one thread reports per window; a lost race just defers to the next. */
    if (!g_last_health_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;

    g_first_health_done.store(true, std::memory_order_relaxed);

    const uint64_t emitted = g_emitted_count.load(std::memory_order_relaxed);
    const uint64_t suppressed = g_suppressed_count.load(std::memory_order_relaxed);
    const uint64_t total = emitted + suppressed;
    const unsigned pct = total ? static_cast<unsigned>((suppressed * 100) / total) : 0;

    const uint64_t gameLines = g_game_line_count.load(std::memory_order_relaxed);

    char line[320];
    const int n = snprintf(line, sizeof(line),
                           "[NEVR.LOGFILTER] health emitted=%llu suppressed=%llu "
                           "suppression_pct=%u game_lines=%llu interval_s=%u",
                           static_cast<unsigned long long>(emitted),
                           static_cast<unsigned long long>(suppressed), pct,
                           static_cast<unsigned long long>(gameLines),
                           kHealthIntervalSec);
    if (n > 0) EmitLine(LOG_LEVEL_INFO, line, n);

    /* Must be a RATE, not a total. Falsified: with the superseded plugin allowed
     * to load, the cumulative count sat at 13 — lines captured in the seconds
     * before the plugin took the hook — so a `== 0` test never fired even though
     * the filter had been inert ever since. What matters is whether game lines
     * are arriving NOW. */
    static uint64_t s_lastGameLines = 0;
    const uint64_t delta = gameLines - s_lastGameLines;
    s_lastGameLines = gameLines;

    if (delta == 0) {
        char warn[320];
        const int wn = snprintf(warn, sizeof(warn),
                                "[NEVR.LOGFILTER] CAPTURED ZERO GAME LINES this interval "
                                "(total=%llu) — the CLog hook is installed but receiving "
                                "nothing. Another module has almost certainly taken the target "
                                "(N89). Filtering, truncation and file logging are all inert.",
                                static_cast<unsigned long long>(gameLines));
        if (wn > 0) EmitLine(LOG_LEVEL_WARNING, warn, wn);
    }
}

/* ------------------------------------------------------------------ */
/* MinHook on CLog::PrintfImpl                                         */
/* ------------------------------------------------------------------ */

typedef void(__fastcall* CLogPrintfImpl_t)(uint32_t level, int64_t category,
                                            const char* fmt, int64_t* varargs);
static CLogPrintfImpl_t orig_PrintfImpl = nullptr;
static void* g_hook_target = nullptr;

static void __fastcall hook_PrintfImpl(uint32_t level, int64_t category,
                                        const char* fmt, int64_t* varargs) {
    if (!g_config.valid || fmt == nullptr) {
        if (orig_PrintfImpl) orig_PrintfImpl(level, category, fmt, varargs);
        return;
    }

    char buf[0x2000];
    int len;

    if (std::strchr(fmt, '%') == nullptr) {
        len = static_cast<int>(std::strlen(fmt));
        if (len >= static_cast<int>(sizeof(buf))) len = sizeof(buf) - 1;
        std::memcpy(buf, fmt, len);
        buf[len] = '\0';
    } else {
        len = vsnprintf(buf, sizeof(buf), fmt, reinterpret_cast<va_list>(varargs));
        if (len < 0) len = 0;
        if (len >= static_cast<int>(sizeof(buf))) len = sizeof(buf) - 1;
        buf[len] = '\0';
    }

    if (len == 0) {
        g_suppressed_count++;
        if (g_config.passthrough_to_engine && orig_PrintfImpl)
            orig_PrintfImpl(level, category, fmt, varargs);
        return;
    }

    if (!(len >= 6 && std::strncmp(buf, "[NEVR.", 6) == 0)) {
        g_game_line_count.fetch_add(1, std::memory_order_relaxed);
    }

    MaybeEmitHealth();  /* N79 */

    if (ShouldSuppress(buf, level)) {
        g_suppressed_count++;
        return;
    }

    int emit_len = ApplyTruncation(buf, len);

    /* N78: collapse repeats instead of deleting them. A closing window emits its
     * summary first so the count appears adjacent to the run it describes. */
    char summary[256];
    const RateVerdict verdict = RateCheck(buf, emit_len, summary, sizeof(summary));
    if (summary[0] != '\0') {
        EmitLine(LOG_LEVEL_INFO, summary, static_cast<int>(std::strlen(summary)));
    }
    if (verdict == RateVerdict::Collapse) {
        g_suppressed_count++;
        return;
    }

    EmitLine(level, buf, emit_len);

    if (g_config.passthrough_to_engine && orig_PrintfImpl) {
        /* N89: max_line_length used to apply ONLY to our JSONL file. The
         * passthrough below re-sent the ORIGINAL fmt+varargs, so the game
         * reformatted the FULL line to console — which is what
         * launch-server.sh captures. Measured: two `[NSUSER] saved ...` profile
         * dumps (5600 and 8192 bytes) were 30.5% of an entire server log while
         * max_line_length was 500. The setting silently did nothing for the
         * output an operator actually reads.
         *
         * When truncation applied, pass the TRUNCATED text through instead.
         * '%' is escaped so the game's formatter performs no conversions and
         * consumes no varargs — passing an already-consumed va_list a second
         * time is what the untruncated branch still does (pre-existing; it
         * works because Win64 va_list is a by-value pointer, but it is not
         * something to add new instances of). */
        if (emit_len < len) {
            char safe[2600];
            int o = 0;
            const int cap = static_cast<int>(sizeof(safe)) - 48;
            for (int i = 0; i < emit_len && o < cap; i++) {
                if (buf[i] == '%') { safe[o++] = '%'; safe[o++] = '%'; }
                else                { safe[o++] = buf[i]; }
            }
            snprintf(safe + o, sizeof(safe) - static_cast<size_t>(o),
                     " ...[+%d bytes truncated]", len - emit_len);
            orig_PrintfImpl(level, category, safe, varargs);
        } else {
            orig_PrintfImpl(level, category, fmt, varargs);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* N90 — pnsrad.dll carries its OWN statically-linked copy of CLog      */
/* ------------------------------------------------------------------ */
/*
 * Hooking echovr.exe's CLog::PrintfImpl does NOT capture pnsrad.dll's logging.
 * Measured: a probe placed before every suppress/truncate decision in our hook
 * never fired for "[NSUSER] saved" while two such lines (5599 and 8191 bytes)
 * reached the console — so the hook was not merely dropping them, it never saw
 * them.
 *
 * `strings` shows the format string "[NSUSER] saved %s: %s" in echovr.exe AND
 * pnsrad.dll AND pnsovr.dll AND pnsdemo.dll — each social-layer DLL links its
 * own copy of the logging code. ReVault: the string lives at 0x180229f10 in
 * pnsrad.dll with one xref at 0x180090ce2, whose call goes to 0x1800929a0 — a
 * varargs wrapper (205 call sites) over 0x1800929c0 (2 call sites), which has
 * the SAME prologue as echovr's CLog::PrintfImpl. A structural mirror.
 *
 * So hook pnsrad.dll's copy at RVA 0x929c0 with the same handler. Prologue is
 * validated first — pnsrad.dll is a relocatable DLL and this address is derived
 * from a different image than the one ReVault indexed.
 */
static constexpr uintptr_t kPnsradPrintfImplRva = 0x929C0;
static constexpr uint8_t kPnsradPrintfImplPrologue[5] = {0x48, 0x89, 0x5C, 0x24, 0x18};
static void* g_pnsrad_hook_target = nullptr;

void BuiltinLogFilter::PollHealth() {
    MaybeEmitHealth();
}

void BuiltinLogFilter::InstallPnsradHook() {
    if (g_pnsrad_hook_target != nullptr) return;  /* already installed */

    /* pnsrad.dll loads well after our init, so this is driven from the per-frame
     * tick and retries until the module appears. Bounded so a build that never
     * loads pnsrad does not retry forever; the give-up is logged once, because a
     * silent give-up is how N86 stayed invisible. */
    static int s_attempts = 0;
    static bool s_gaveUp = false;
    if (s_gaveUp) return;

    HMODULE hPnsrad = GetModuleHandleA("pnsrad.dll");
    if (hPnsrad == nullptr) {
        if (++s_attempts > 4000) {  /* ~32s at 125Hz */
            s_gaveUp = true;
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.PATCH] pnsrad.dll never loaded after %d attempts — its log copy is "
                "NOT filtered; its output bypasses max_line_length (N90)",
                s_attempts);
        }
        return;
    }

    void* target = reinterpret_cast<uint8_t*>(hPnsrad) + kPnsradPrintfImplRva;
    uint8_t actual[5] = {0};
    memcpy(actual, target, sizeof(actual));
    if (memcmp(actual, kPnsradPrintfImplPrologue, sizeof(actual)) != 0) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] hook skipped name=pnsrad!CLog::PrintfImpl rva=0x%llX "
            "reason=prologue_mismatch expected=48895c2418 actual=%02x%02x%02x%02x%02x (N90)",
            static_cast<unsigned long long>(kPnsradPrintfImplRva),
            actual[0], actual[1], actual[2], actual[3], actual[4]);
        return;
    }

    /* Same handler: the signature is identical, and the filter is stateless with
     * respect to which image produced the line. */
    void* orig = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hook_PrintfImpl), &orig) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] hook failed name=pnsrad!CLog::PrintfImpl rva=0x%llX (N90)",
            static_cast<unsigned long long>(kPnsradPrintfImplRva));
        return;
    }

    g_pnsrad_hook_target = target;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] hooked name=pnsrad!CLog::PrintfImpl rva=0x%llX — pnsrad log output "
        "now filtered and truncated (N90)",
        static_cast<unsigned long long>(kPnsradPrintfImplRva));
}

void BuiltinLogFilter::Init(uintptr_t base_addr, bool is_server) {
    BlfLog("initializing (base=0x%llx, server=%s)",
           static_cast<unsigned long long>(base_addr),
           is_server ? "true" : "false");

    g_base_addr = base_addr;

    /* Load config or use defaults */
    std::string config_path = FindConfigFile();
    if (config_path.empty()) {
        g_config = MakeDefaultConfig();
        BlfLog("no config file found, using built-in defaults (%zu suppress patterns)",
               g_config.suppress_patterns.size());
    } else {
        LogFilterConfig cfg = LoadConfigFromFile(config_path.c_str());
        if (!cfg.valid) {
            BlfLog("config parse failed, using built-in defaults");
            g_config = MakeDefaultConfig();
        } else {
            g_config = cfg;
            BlfLog("config loaded: min_level=%u, %zu channels, %zu patterns, %zu truncate, "
                   "file=%s(%s), color=%s, rotate=%s",
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

    /* Set up file logging */
    InitFileLogging();

    /* Install hook on CLog::PrintfImpl */
    g_hook_target = nevr::ResolveVA_Checked(base_addr, nevr::addresses::VA_CLOG_PRINTF_IMPL);

    MH_STATUS status = MH_CreateHook(g_hook_target,
                                      reinterpret_cast<void*>(&hook_PrintfImpl),
                                      reinterpret_cast<void**>(&orig_PrintfImpl));
    if (status != MH_OK) {
        uint8_t actual[4] = {0};
        memcpy(actual, g_hook_target, 4);
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] hook failed name=CLog::PrintfImpl va=0x%llX "
            "expected=00000000 actual=%02x%02x%02x%02x status=%d",
            static_cast<unsigned long long>(nevr::addresses::VA_CLOG_PRINTF_IMPL),
            actual[0], actual[1], actual[2], actual[3],
            static_cast<int>(status));
        return;
    }

    status = MH_EnableHook(g_hook_target);
    if (status != MH_OK) {
        uint8_t actual[4] = {0};
        memcpy(actual, g_hook_target, 4);
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] hook failed name=CLog::PrintfImpl va=0x%llX "
            "expected=00000000 actual=%02x%02x%02x%02x status=%d",
            static_cast<unsigned long long>(nevr::addresses::VA_CLOG_PRINTF_IMPL),
            actual[0], actual[1], actual[2], actual[3],
            static_cast<int>(status));
        return;
    }

    Log(EchoVR::LogLevel::Debug,
        "[NEVR.PATCH] hooked name=CLog::PrintfImpl va=0x%llX",
        static_cast<unsigned long long>(nevr::addresses::VA_CLOG_PRINTF_IMPL));
}

void BuiltinLogFilter::Shutdown() {
    BlfLog("shutting down");

    if (g_hook_target) {
        MH_DisableHook(g_hook_target);
        MH_RemoveHook(g_hook_target);
        g_hook_target = nullptr;
        BlfLog("hook removed (emitted: %llu, suppressed: %llu)",
               static_cast<unsigned long long>(g_emitted_count.load()),
               static_cast<unsigned long long>(g_suppressed_count.load()));
    }

    ShutdownFileLogging();
}
