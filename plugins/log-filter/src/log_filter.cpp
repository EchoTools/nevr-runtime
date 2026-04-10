/* SYNTHESIS -- custom tool code, not from binary */

#include "log_filter.h"
#include "nevr_plugin_interface.h"

#include <MinHook.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

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
/* Logging (bootstrap — before hook is installed)                      */
/* ------------------------------------------------------------------ */

static void PluginLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[log_filter] ");
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

/* File output state */
static FILE* g_log_file = nullptr;
static std::mutex g_file_mutex;
static std::string g_log_file_path;
static uint64_t g_file_bytes_written = 0;
static uint64_t g_file_open_time = 0;       /* seconds since epoch */
static std::string g_log_file_prefix;       /* e.g. "echovr-server-12345" */
static std::string g_log_dir;

/* va_list must be a simple pointer on x64 for our int64_t* cast to work */
static_assert(sizeof(va_list) == sizeof(void*),
              "va_list must be a pointer type for int64_t*-to-va_list cast");

/* ------------------------------------------------------------------ */
/* Platform helpers                                                    */
/* ------------------------------------------------------------------ */

static uint32_t GetPid() {
#ifdef _WIN32
    return static_cast<uint32_t>(_getpid());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

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

LogFilterConfig ParseLogFilterConfigString(const std::string& json_str) {
    std::string clean = StripComments(json_str);
    LogFilterConfig cfg;

    try {
        auto j = nlohmann::json::parse(clean);

        cfg.min_level = j.value("min_level", cfg.min_level);
        cfg.max_line_length = j.value("max_line_length", cfg.max_line_length);
        cfg.timestamps = j.value("timestamps", cfg.timestamps);
        cfg.passthrough_to_engine = j.value("passthrough_to_engine", cfg.passthrough_to_engine);

        /* Console */
        cfg.console_enabled = j.value("console_enabled", cfg.console_enabled);
        cfg.console_color = j.value("console_color", cfg.console_color);

        /* File */
        cfg.file_enabled = j.value("file_enabled", cfg.file_enabled);
        cfg.file_dir = j.value("file_dir", cfg.file_dir);
        cfg.file_jsonl = j.value("file_jsonl", cfg.file_jsonl);

        /* Rotation */
        cfg.rotate_enabled = j.value("rotate_enabled", cfg.rotate_enabled);
        cfg.rotate_max_size_mb = j.value("rotate_max_size_mb", cfg.rotate_max_size_mb);
        cfg.rotate_interval_min = j.value("rotate_interval_min", cfg.rotate_interval_min);
        cfg.rotate_keep = j.value("rotate_keep", cfg.rotate_keep);

        /* String arrays */
        if (j.contains("suppress_channels")) {
            for (const auto& s : j["suppress_channels"])
                cfg.suppress_channels.push_back(s.get<std::string>());
        }
        if (j.contains("suppress_patterns")) {
            for (const auto& s : j["suppress_patterns"])
                cfg.suppress_patterns.push_back(s.get<std::string>());
        }

        /* Truncate rules */
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
        return cfg; /* return defaults on parse failure */
    }

    if (cfg.max_line_length < 0) cfg.max_line_length = 0;
    cfg.valid = true;
    return cfg;
}

LogFilterConfig ParseYamlLogFilterConfig(const std::string& yaml_str) {
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

LogFilterConfig LoadLogFilterConfig(const char* path) {
    std::string content = nevr::LoadConfigFile(path);
    if (content.empty()) {
        PluginLog("failed to read config file: %s", path);
        return {};
    }
    std::string p(path);
    if ((p.size() >= 4 && p.substr(p.size() - 4) == ".yml") ||
        (p.size() >= 5 && p.substr(p.size() - 5) == ".yaml")) {
        return ParseYamlLogFilterConfig(content);
    }
    return ParseLogFilterConfigString(content);
}

void SetLogFilterConfig(const LogFilterConfig& cfg) {
    g_config = cfg;
}

/* ------------------------------------------------------------------ */
/* Timestamp helper                                                    */
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

/* Filename-safe timestamp: 2026-03-16T23-23-55.204 */
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
        PluginLog("failed to open log file: %s", g_log_file_path.c_str());
        return;
    }
    g_file_bytes_written = 0;
    g_file_open_time = GetEpochSeconds();
    PluginLog("logging to: %s", g_log_file_path.c_str());
}

static void CloseLogFile() {
    if (g_log_file) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

/* Delete oldest log files if we exceed rotate_keep */
static void PruneOldLogs() {
    if (g_config.rotate_keep <= 0) return;

    /* Collect matching files in log dir */
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

    /* Sort lexicographically (timestamp in name makes this chronological) */
    std::sort(files.begin(), files.end());

    /* Delete oldest files beyond keep limit (subtract 1 for the current file) */
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

void InitFileLogging() {
    if (!g_config.file_enabled) return;

    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%u", GetPid());
    g_log_file_prefix = std::string("echovr-server-") + pid_str;

    if (g_config.file_dir.empty()) {
        /* Default: logs/ subdirectory next to the binary */
#ifdef _WIN32
        char mod_path[MAX_PATH];
        GetModuleFileNameA(nullptr, mod_path, MAX_PATH);
        std::string dir(mod_path);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        g_log_dir = dir + "\\logs";
        CreateDirectoryA(g_log_dir.c_str(), nullptr);
#else
        g_log_dir = "logs";
        mkdir(g_log_dir.c_str(), 0755);
#endif
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

void ShutdownFileLogging() {
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

static bool ShouldSuppress(const char* message, uint32_t level) {
    if (!g_config.valid) return false;

    if (level < g_config.min_level && level != LOG_LEVEL_RAW) return true;

    for (const auto& ch : g_config.suppress_channels) {
        if (std::strncmp(message, ch.c_str(), ch.size()) == 0) return true;
    }

    for (const auto& pat : g_config.suppress_patterns) {
        if (std::strstr(message, pat.c_str()) != nullptr) return true;
    }

    return false;
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
        case LOG_LEVEL_DEBUG:   return "\033[36m";      /* cyan */
        case LOG_LEVEL_INFO:    return "\033[0m";       /* default */
        case LOG_LEVEL_WARNING: return "\033[33m";      /* yellow */
        case LOG_LEVEL_ERROR:   return "\033[31m";      /* red */
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
            /* JSONL: {"ts":"...","level":"...","msg":"..."} */
            std::string line = "{\"ts\":\"";
            line += ts;
            line += "\",\"level\":\"";
            line += lvl;
            line += "\",\"msg\":\"";
            JsonEscapeAppend(line, message, len);
            line += "\"}\n";

            size_t written = std::fwrite(line.data(), 1, line.size(), g_log_file);
            g_file_bytes_written += written;
        } else {
            /* Plain text */
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
/* MinHook on CLog::PrintfImpl @ 0x1400ebe70                          */
/* ------------------------------------------------------------------ */

typedef void(__fastcall* CLogPrintfImpl_t)(uint32_t level, int64_t category,
                                            const char* fmt, int64_t* varargs);
static CLogPrintfImpl_t orig_PrintfImpl = nullptr;

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

    if (ShouldSuppress(buf, level)) {
        g_suppressed_count++;
        return;
    }

    int emit_len = ApplyTruncation(buf, len);

    EmitLine(level, buf, emit_len);

    if (g_config.passthrough_to_engine && orig_PrintfImpl) {
        orig_PrintfImpl(level, category, fmt, varargs);
    }
}

bool InstallLogFilterHook(uintptr_t base_addr) {
    g_base_addr = base_addr;

    /* Initialize file logging if configured */
    InitFileLogging();

    MH_Initialize();

    auto* target = nevr::ResolveVA(base_addr, nevr::addresses::VA_CLOG_PRINTF_IMPL);
    MH_STATUS status = MH_CreateHook(target,
                                      reinterpret_cast<void*>(&hook_PrintfImpl),
                                      reinterpret_cast<void**>(&orig_PrintfImpl));
    if (status != MH_OK) {
        PluginLog("MH_CreateHook failed for CLog::PrintfImpl: %d", status);
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        PluginLog("MH_EnableHook failed: %d", status);
        return false;
    }

    PluginLog("hook installed on CLog::PrintfImpl @ 0x%llx",
              static_cast<unsigned long long>(nevr::addresses::VA_CLOG_PRINTF_IMPL));
    return true;
}

void RemoveLogFilterHook() {
    if (g_base_addr != 0) {
        auto* target = nevr::ResolveVA(g_base_addr, nevr::addresses::VA_CLOG_PRINTF_IMPL);
        MH_DisableHook(target);
        MH_RemoveHook(target);
        PluginLog("hook removed (emitted: %llu, suppressed: %llu)",
                  static_cast<unsigned long long>(g_emitted_count.load()),
                  static_cast<unsigned long long>(g_suppressed_count.load()));
    }
    ShutdownFileLogging();
}
