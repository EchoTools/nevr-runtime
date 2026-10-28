/*
 * plugin_logger.h — Shared plugin logging macro (N135: lines go to the host's Log() once bound).
 *
 * Provides NEVR_DEFINE_PLUGIN_LOG(prefix) which generates inline functions that write one leveled
 * line per call:
 *
 *   PluginLogBind(ctx)                   bind to the host's log; call first thing in init
 *   PluginLogDebug(fmt, ...)             printf-style message at NEVR_LOG_DEBUG
 *   PluginLog(fmt, ...)                  printf-style message at NEVR_LOG_INFO
 *   PluginLogWarning(fmt, ...)           printf-style message at NEVR_LOG_WARNING
 *   PluginLogError(fmt, ...)             printf-style message at NEVR_LOG_ERROR
 *   PluginLogFields(level, msg, {{k,v}}) plain message plus key=value fields
 *
 * Where the line goes. Once PluginLogBind(ctx) has seen a context from a v6+ host (ctx_size covers
 * ctx->log, and the host filled plugin_id and log), every call goes to the host's structured Log():
 * the host tags it [NEVR.PLUGIN] plugin=<id>, with the id it knows the plugin by (its config.yaml
 * entry's name), at the level the call names, in the host's own log file. `prefix` is not used then.
 * Before PluginLogBind, and on a host that predates v6, the line is "<prefix> <LEVEL> <message>" on
 * stderr, which the host does not collect.
 *
 * When. A plugin has no host log before its init callback (there is no context yet), and must not
 * log from DllMain (the loader lock is held there). Bind at the top of NvrPluginInit or
 * NvrPluginInitEx; the binding then also serves OnFrame, OnGameStateChange and Shutdown, from any
 * thread. A failure is PluginLogWarning or PluginLogError, never PluginLog.
 *
 * Invoke once per plugin, optionally inside a namespace for multi-TU plugins.
 *
 * Example (single-TU plugin):
 *   NEVR_DEFINE_PLUGIN_LOG("[my_plugin]")
 *   NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
 *       PluginLogBind(ctx);
 *       PluginLogFields(NEVR_LOG_INFO, "initialized", {{"hooks", "3"}});
 *       return 0;
 *   }
 *
 * Example (multi-TU plugin, in a shared header):
 *   namespace my_plugin { NEVR_DEFINE_PLUGIN_LOG("[my_plugin]") }
 *   // Call as my_plugin::PluginLog("msg %d", val);
 */
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#include "nevr/plugin_interface.h"

namespace nevr {

/// What a plugin keeps from the context it was given at init: the host's name for it and the host's log.
struct PluginLogBinding {
    const char* id = nullptr;
    NevrLogFn log = nullptr;
};

/// Binds to the host's log when `ctx` carries one: it is from a v6+ host (ctx_size reaches past ctx->log),
/// and the host filled plugin_id and log. Otherwise the binding is cleared and logging stays on stderr.
inline bool BindPluginLog(PluginLogBinding& binding, const NvrGameContext* ctx) {
    binding = PluginLogBinding{};
    if (ctx == nullptr) return false;
    constexpr std::size_t needed = offsetof(NvrGameContext, log) + sizeof(NevrLogFn);
    if (ctx->ctx_size < needed || ctx->plugin_id == nullptr || ctx->log == nullptr) return false;
    binding.id = ctx->plugin_id;
    binding.log = ctx->log;
    return true;
}

inline const char* PluginLogLevelName(uint32_t level) {
    switch (level) {
        case NEVR_LOG_DEBUG: return "DEBUG";
        case NEVR_LOG_INFO: return "INFO";
        case NEVR_LOG_WARNING: return "WARNING";
        case NEVR_LOG_ERROR: return "ERROR";
        default: return "LEVEL?";
    }
}

/// The printf-style message, formatted, with no prefix or level.
inline std::string FormatPluginMessage(const char* fmt, va_list args) {
    va_list measure;
    va_copy(measure, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);
    std::string message(needed > 0 ? static_cast<std::size_t>(needed) : 0U, '\0');
    if (needed > 0) std::vsnprintf(&message[0], message.size() + 1, fmt, args);
    return message;
}

/// The line a plugin logger writes to stderr, without the trailing newline: "<prefix> <LEVEL> <message>".
inline std::string FormatPluginLog(const char* prefix, const char* level, const char* fmt, va_list args) {
    return std::string(prefix) + " " + level + " " + FormatPluginMessage(fmt, args);
}

inline void WritePluginLog(const char* prefix, const char* level, const char* fmt, va_list args) {
    const std::string line = FormatPluginLog(prefix, level, fmt, args);
    std::fprintf(stderr, "%s\n", line.c_str());
}

/// One line with fields: through the host's log when bound, otherwise to stderr as
/// "<prefix> <LEVEL> <message> key=value ...".
inline void EmitPluginLog(const PluginLogBinding& binding, const char* prefix, uint32_t level,
                          const char* message, const NevrLogField* fields, uint32_t fieldCount) {
    if (binding.log != nullptr) {
        binding.log(binding.id, level, message, fields, fieldCount);
        return;
    }
    std::string line = std::string(prefix) + " " + PluginLogLevelName(level) + " " + message;
    for (uint32_t i = 0; fields != nullptr && i < fieldCount; ++i) {
        if (fields[i].key == nullptr) continue;
        line += std::string(" ") + fields[i].key + "=" + (fields[i].value != nullptr ? fields[i].value : "");
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}

inline void EmitPluginLogV(const PluginLogBinding& binding, const char* prefix, uint32_t level,
                           const char* fmt, va_list args) {
    EmitPluginLog(binding, prefix, level, FormatPluginMessage(fmt, args).c_str(), nullptr, 0);
}

}  // namespace nevr

#define NEVR_DEFINE_PLUGIN_LOG(prefix)                                                     \
    inline ::nevr::PluginLogBinding& PluginLogState() {                                    \
        static ::nevr::PluginLogBinding binding;                                           \
        return binding;                                                                    \
    }                                                                                      \
    inline bool PluginLogBind(const NvrGameContext* ctx) {                                 \
        return ::nevr::BindPluginLog(PluginLogState(), ctx);                               \
    }                                                                                      \
    inline void PluginLogFields(uint32_t level, const char* message,                       \
                                std::initializer_list<NevrLogField> fields) {              \
        ::nevr::EmitPluginLog(PluginLogState(), prefix, level, message, fields.begin(),    \
                              static_cast<uint32_t>(fields.size()));                       \
    }                                                                                      \
    inline void PluginLogDebug(const char* fmt, ...) {                                     \
        va_list args;                                                                      \
        va_start(args, fmt);                                                               \
        ::nevr::EmitPluginLogV(PluginLogState(), prefix, NEVR_LOG_DEBUG, fmt, args);       \
        va_end(args);                                                                      \
    }                                                                                      \
    inline void PluginLog(const char* fmt, ...) {                                          \
        va_list args;                                                                      \
        va_start(args, fmt);                                                               \
        ::nevr::EmitPluginLogV(PluginLogState(), prefix, NEVR_LOG_INFO, fmt, args);        \
        va_end(args);                                                                      \
    }                                                                                      \
    inline void PluginLogWarning(const char* fmt, ...) {                                   \
        va_list args;                                                                      \
        va_start(args, fmt);                                                               \
        ::nevr::EmitPluginLogV(PluginLogState(), prefix, NEVR_LOG_WARNING, fmt, args);     \
        va_end(args);                                                                      \
    }                                                                                      \
    inline void PluginLogError(const char* fmt, ...) {                                     \
        va_list args;                                                                      \
        va_start(args, fmt);                                                               \
        ::nevr::EmitPluginLogV(PluginLogState(), prefix, NEVR_LOG_ERROR, fmt, args);       \
        va_end(args);                                                                      \
    }
