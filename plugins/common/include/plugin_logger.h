/*
 * plugin_logger.h — Shared plugin logging macro.
 *
 * Provides NEVR_DEFINE_PLUGIN_LOG(prefix) which generates three inline
 * functions writing one leveled line each to stderr:
 *
 *   PluginLog(fmt, ...)         "[prefix] INFO message"
 *   PluginLogWarning(fmt, ...)  "[prefix] WARNING message"
 *   PluginLogError(fmt, ...)    "[prefix] ERROR message"
 *
 * A plugin is a separate DLL and cannot call the host's structured Log(), so
 * its lines stay on stderr; the level token makes them filterable. A failure
 * is PluginLogWarning or PluginLogError, never PluginLog.
 *
 * Invoke once per plugin, optionally inside a namespace for multi-TU plugins.
 *
 * Example (single-TU plugin):
 *   NEVR_DEFINE_PLUGIN_LOG("[my_plugin]")
 *
 * Example (multi-TU plugin, in a shared header):
 *   namespace my_plugin { NEVR_DEFINE_PLUGIN_LOG("[my_plugin]") }
 *   // Call as my_plugin::PluginLog("msg %d", val);
 */
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace nevr {

/// The line a plugin logger writes, without the trailing newline: "<prefix> <LEVEL> <message>".
inline std::string FormatPluginLog(const char* prefix, const char* level, const char* fmt, va_list args) {
    va_list measure;
    va_copy(measure, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);
    std::string message(needed > 0 ? static_cast<std::size_t>(needed) : 0U, '\0');
    if (needed > 0) std::vsnprintf(&message[0], message.size() + 1, fmt, args);
    return std::string(prefix) + " " + level + " " + message;
}

inline void WritePluginLog(const char* prefix, const char* level, const char* fmt, va_list args) {
    const std::string line = FormatPluginLog(prefix, level, fmt, args);
    std::fprintf(stderr, "%s\n", line.c_str());
}

}  // namespace nevr

#define NEVR_DEFINE_PLUGIN_LOG(prefix)                                       \
    inline void PluginLog(const char* fmt, ...) {                            \
        va_list args;                                                        \
        va_start(args, fmt);                                                 \
        ::nevr::WritePluginLog(prefix, "INFO", fmt, args);                   \
        va_end(args);                                                        \
    }                                                                        \
    inline void PluginLogWarning(const char* fmt, ...) {                     \
        va_list args;                                                        \
        va_start(args, fmt);                                                 \
        ::nevr::WritePluginLog(prefix, "WARNING", fmt, args);                \
        va_end(args);                                                        \
    }                                                                        \
    inline void PluginLogError(const char* fmt, ...) {                       \
        va_list args;                                                        \
        va_start(args, fmt);                                                 \
        ::nevr::WritePluginLog(prefix, "ERROR", fmt, args);                  \
        va_end(args);                                                        \
    }
