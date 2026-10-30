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
 *
 * Lines logged before PluginLogBind are held in memory, copied, in order, and PluginLogBind writes
 * them through the host's log when it binds. Only a bind that finds no host log (a host that predates
 * v6, or a context without ctx->log) writes them to stderr, which the host does not collect; that
 * bind says so on stderr first. The held lines are capped at kPluginLogHeldLines and
 * kPluginLogHeldBytes; a line past the cap is counted, and the bind reports the count. Held lines
 * have no lock: bind from the init thread before any other thread logs.
 *
 * Stderr line form: "<prefix> <LEVEL> <message>", with " key=value" for each field.
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
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "nevr/plugin_interface.h"

namespace nevr {

/// What a plugin keeps from the context it was given at init: the host's name for it and the host's log,
/// and the lines logged before the bind decided where they go. A default-constructed binding is Pending.
struct PluginLogBinding {
    enum class Stage { Pending, Bound, Unbound };

    /// One line logged while Pending: its own copies of the message and of each field.
    struct HeldLine {
        uint32_t level = 0;
        std::string message;
        std::vector<std::pair<std::string, std::string>> fields;
    };

    const char* id = nullptr;
    NevrLogFn log = nullptr;
    Stage stage = Stage::Pending;
    std::vector<HeldLine> held;
    std::size_t heldBytes = 0;
    std::size_t heldDropped = 0;
};

/// The most lines, and the most bytes of text, a plugin may hold before its bind. A plugin logs a
/// handful of lines before it binds; the caps stop a runaway init from growing the buffer without bound.
constexpr std::size_t kPluginLogHeldLines = 256;
constexpr std::size_t kPluginLogHeldBytes = 64 * 1024;

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

/// One line to stderr as "<prefix> <LEVEL> <message> key=value ...".
inline void WriteLineToStderr(const char* prefix, uint32_t level, const char* message, const NevrLogField* fields,
                              uint32_t fieldCount) {
    std::string line = std::string(prefix) + " " + PluginLogLevelName(level) + " " + message;
    for (uint32_t i = 0; fields != nullptr && i < fieldCount; ++i) {
        if (fields[i].key == nullptr) continue;
        line += std::string(" ") + fields[i].key + "=" + (fields[i].value != nullptr ? fields[i].value : "");
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}

/// One line to the host's log when the binding has one, otherwise to stderr.
inline void EmitToBinding(const PluginLogBinding& binding, const char* prefix, uint32_t level, const char* message,
                          const NevrLogField* fields, uint32_t fieldCount) {
    if (binding.log != nullptr) {
        binding.log(binding.id, level, message, fields, fieldCount);
        return;
    }
    WriteLineToStderr(prefix, level, message, fields, fieldCount);
}

/// Holds one line logged before the bind, copying its text; past the caps it is counted, not held.
inline void HoldPluginLine(PluginLogBinding& binding, uint32_t level, const char* message, const NevrLogField* fields,
                           uint32_t fieldCount) {
    PluginLogBinding::HeldLine line;
    line.level = level;
    line.message = message;
    std::size_t bytes = line.message.size();
    for (uint32_t i = 0; fields != nullptr && i < fieldCount; ++i) {
        if (fields[i].key == nullptr) continue;
        std::string value = fields[i].value != nullptr ? fields[i].value : "";
        bytes += std::strlen(fields[i].key) + value.size();
        line.fields.emplace_back(fields[i].key, std::move(value));
    }
    if (binding.held.size() >= kPluginLogHeldLines || binding.heldBytes + bytes > kPluginLogHeldBytes) {
        ++binding.heldDropped;
        return;
    }
    binding.heldBytes += bytes;
    binding.held.push_back(std::move(line));
}

/// Writes the held lines in order, now that the bind has decided where they go, then reports the count
/// the caps dropped. With no host log the held lines go to stderr, after a line that says why.
inline void FlushHeldPluginLines(PluginLogBinding& binding, const char* prefix) {
    std::vector<PluginLogBinding::HeldLine> held = std::move(binding.held);
    const std::size_t dropped = binding.heldDropped;
    binding.held.clear();
    binding.heldBytes = 0;
    binding.heldDropped = 0;
    if (!held.empty() && binding.log == nullptr) {
        WriteLineToStderr(prefix, NEVR_LOG_WARNING,
                          "no host log function; the lines logged before bind follow on stderr", nullptr, 0);
    }
    for (const PluginLogBinding::HeldLine& line : held) {
        std::vector<NevrLogField> fields;
        fields.reserve(line.fields.size());
        for (const auto& field : line.fields) fields.push_back(NevrLogField{field.first.c_str(), field.second.c_str()});
        EmitToBinding(binding, prefix, line.level, line.message.c_str(), fields.data(),
                      static_cast<uint32_t>(fields.size()));
    }
    if (dropped > 0) {
        const std::string count = std::to_string(dropped);
        const NevrLogField field{"count", count.c_str()};
        EmitToBinding(binding, prefix, NEVR_LOG_WARNING, "lines logged before bind were dropped (buffer full)", &field,
                      1);
    }
}

/// Binds to the host's log when `ctx` carries one: it is from a v6+ host (ctx_size reaches past ctx->log),
/// and the host filled plugin_id and log. Otherwise the binding stays on stderr. Either way the held lines
/// are written now, in order, through whichever sink the bind chose.
inline bool BindPluginLog(PluginLogBinding& binding, const char* prefix, const NvrGameContext* ctx) {
    bool bound = false;
    if (ctx != nullptr) {
        constexpr std::size_t needed = offsetof(NvrGameContext, log) + sizeof(NevrLogFn);
        bound = ctx->ctx_size >= needed && ctx->plugin_id != nullptr && ctx->log != nullptr;
    }
    binding.id = bound ? ctx->plugin_id : nullptr;
    binding.log = bound ? ctx->log : nullptr;
    binding.stage = bound ? PluginLogBinding::Stage::Bound : PluginLogBinding::Stage::Unbound;
    FlushHeldPluginLines(binding, prefix);
    return bound;
}

/// One line with fields: held while the binding is Pending, otherwise through the host's log when bound and
/// to stderr when not.
inline void EmitPluginLog(PluginLogBinding& binding, const char* prefix, uint32_t level, const char* message,
                          const NevrLogField* fields, uint32_t fieldCount) {
    if (binding.stage == PluginLogBinding::Stage::Pending) {
        HoldPluginLine(binding, level, message, fields, fieldCount);
        return;
    }
    EmitToBinding(binding, prefix, level, message, fields, fieldCount);
}

inline void EmitPluginLogV(PluginLogBinding& binding, const char* prefix, uint32_t level, const char* fmt,
                           va_list args) {
    EmitPluginLog(binding, prefix, level, FormatPluginMessage(fmt, args).c_str(), nullptr, 0);
}

}  // namespace nevr

#define NEVR_DEFINE_PLUGIN_LOG(prefix)                                                     \
    inline ::nevr::PluginLogBinding& PluginLogState() {                                    \
        static ::nevr::PluginLogBinding binding;                                           \
        return binding;                                                                    \
    }                                                                                      \
    inline bool PluginLogBind(const NvrGameContext* ctx) {                                 \
        return ::nevr::BindPluginLog(PluginLogState(), prefix, ctx);                       \
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
