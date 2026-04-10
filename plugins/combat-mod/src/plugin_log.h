/*
 * plugin_log.h — Plugin-local logging for combat-mod.
 *
 * Plugins can't link libcommon.a (that's part of gamepatches), so they
 * can't use Log(EchoVR::LogLevel::Info, ...). Use fprintf(stderr, ...)
 * matching the pattern in log-filter and server-timing plugins.
 */
#pragma once

#include <cstdio>
#include <cstdarg>

namespace combat_mod {

inline void PluginLog(const char* fmt, ...) {
    std::fprintf(stderr, "[NEVR.COMBAT] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

} // namespace combat_mod
