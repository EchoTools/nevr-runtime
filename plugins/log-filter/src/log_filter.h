/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

/*
 * Log Filter — Replace echovr.exe logging with structured, filtered output.
 *
 * Hooks CLog::PrintfImpl @ 0x1400ebe70 (NOT the variadic wrapper at 0x1400ebe50).
 * PrintfImpl is a regular 4-argument __fastcall:
 *   void(uint32_t level, int64_t category, const char* fmt, int64_t* varargs)
 *
 * The hook formats the message, checks against suppression rules, and emits
 * structured output to console (with optional ANSI color) and/or a log file
 * (plain text or JSONL). Log files are per-process with optional rotation.
 *
 * Source: echovr-reconstruction src/NRadEngine/Core/CLog.h, CLog.cpp
 * Confidence: M (address verified, reconstruction quality M)
 */

/* Engine log levels (bitmask, from CLog.h ELogLevel) */
constexpr uint32_t LOG_LEVEL_DEBUG   = 1;
constexpr uint32_t LOG_LEVEL_INFO    = 2;    /* Most messages, no prefix in engine */
constexpr uint32_t LOG_LEVEL_WARNING = 4;
constexpr uint32_t LOG_LEVEL_ERROR   = 8;
constexpr uint32_t LOG_LEVEL_RAW     = 1000; /* No prefix, no newline */

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

    /* Console output */
    bool        console_enabled = true;
    bool        console_color = true;       /* ANSI color codes on console */

    /* File output */
    bool        file_enabled = false;
    std::string file_dir;                   /* directory for log files (default: next to binary) */
    bool        file_jsonl = true;          /* true=JSONL, false=plain text */

    /* Rotation */
    bool        rotate_enabled = false;
    int         rotate_max_size_mb = 50;    /* rotate when file exceeds this size (0=no size limit) */
    int         rotate_interval_min = 0;    /* rotate every N minutes (0=no time limit) */
    int         rotate_keep = 5;            /* number of old log files to keep (0=unlimited) */

    bool        valid = false;
};

LogFilterConfig LoadLogFilterConfig(const char* path);
LogFilterConfig ParseLogFilterConfigString(const std::string& json_str);
/* Must be called BEFORE InstallLogFilterHook — not thread-safe. */
void SetLogFilterConfig(const LogFilterConfig& cfg);
bool InstallLogFilterHook(uintptr_t base_addr);
void RemoveLogFilterHook();
