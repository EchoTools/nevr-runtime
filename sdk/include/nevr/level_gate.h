/**
 * level_gate.h — switch a plugin on for the levels its config names (plugin API v8, #1094).
 *
 * The host hands every context the game's main level as ctx->level_symbol (a CSymbol64) and, when its symbol
 * table knows the level, ctx->level_name. A level a plugin registered itself is not in the table, so the gate
 * compares symbols: it hashes the names the plugin's own config.yaml entry gave it once, at init, and checks the
 * context's symbol against them.
 *
 *   static LevelGate g_gate;
 *
 *   NvrPluginInitEx(ctx, args_json):  g_gate.Set(levels read from args_json);
 *   NvrPluginOnFrame(ctx):            const bool on = g_gate.Allows(ctx);      // act only while `on`
 *   NvrPluginOnGameStateChange(ctx):  const bool on = g_gate.Allows(ctx);      // a level can also load with
 *                                                                              // no state change, so decide
 *                                                                              // per frame as well
 *
 * An empty list means "every level": the gate is open whatever the host reports. A plugin that names levels and
 * runs on a host older than v8 gets a closed gate (Covers() is false), so it never acts on a level it cannot see.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "nevr/plugin_interface.h"

class LevelGate {
public:
    /// The levels, by name, this plugin is for. Replaces any earlier list.
    void Set(const std::vector<std::string>& names) {
        symbols_.clear();
        for (const std::string& name : names) {
            if (!name.empty()) symbols_.push_back(EchoVR::CSymbol64Hash(name.c_str()));
        }
    }

    /// True once Set was given at least one name.
    bool Configured() const { return !symbols_.empty(); }

    /// True when `ctx` came from a host that reports the level (v8+): its ctx_size reaches past level_name.
    static bool Covers(const NvrGameContext* ctx) {
        return ctx != nullptr &&
               ctx->ctx_size >= offsetof(NvrGameContext, level_name) + sizeof(ctx->level_name);
    }

    /// Whether the plugin should act in this context: no names configured, or the context's level is one of them.
    bool Allows(const NvrGameContext* ctx) const {
        if (!Configured()) return true;
        if (!Covers(ctx)) return false;
        for (const uint64_t symbol : symbols_) {
            if (symbol == ctx->level_symbol) return true;
        }
        return false;
    }

private:
    std::vector<uint64_t> symbols_;
};
