/*
 * plugin_logger.h — Shared plugin logging macro.
 *
 * Provides NEVR_DEFINE_PLUGIN_LOG(prefix) which generates an inline
 * PluginLog() function writing to stderr. Invoke once per plugin,
 * optionally inside a namespace for multi-TU plugins.
 *
 * Example (single-TU plugin):
 *   NEVR_DEFINE_PLUGIN_LOG("[my_plugin]")
 *
 * Example (multi-TU plugin, in a shared header):
 *   namespace my_plugin { NEVR_DEFINE_PLUGIN_LOG("[my_plugin]") }
 *   // Call as my_plugin::PluginLog("msg %d", val);
 */
#pragma once

#include <cstdio>
#include <cstdarg>

#define NEVR_DEFINE_PLUGIN_LOG(prefix)                          \
    inline void PluginLog(const char* fmt, ...) {               \
        std::fprintf(stderr, prefix " ");                       \
        va_list args;                                           \
        va_start(args, fmt);                                    \
        std::vfprintf(stderr, fmt, args);                       \
        va_end(args);                                           \
        std::fprintf(stderr, "\n");                             \
    }
