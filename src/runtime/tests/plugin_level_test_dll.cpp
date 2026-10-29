// #1094 fixture: a plugin that reads the level from its init, per-frame and state-change contexts, keeps the
// last one it saw in each, and logs the level on every state change through the host log.

#include "nevr/plugin_interface.h"

#include <cstddef>
#include <cstdio>

namespace {

constexpr NvrPluginInfo kInfo = {
    "test-plugin-level",
    "Hermetic plugin-loader fixture that reads the current level",
    1u,
    0u,
    0u,
};

struct Seen {
  bool covered = false;  // the context's ctx_size reached past level_name
  uint64_t symbol = 0;
  const char* name = nullptr;
  uint32_t calls = 0;
};

Seen g_init;
Seen g_frame;
Seen g_state;

bool CtxCoversLevel(const NvrGameContext* ctx) {
  return ctx != nullptr && ctx->ctx_size >= offsetof(NvrGameContext, level_name) + sizeof(ctx->level_name);
}

void Record(Seen& seen, const NvrGameContext* ctx) {
  seen.covered = CtxCoversLevel(ctx);
  seen.symbol = seen.covered ? ctx->level_symbol : 0;
  seen.name = seen.covered ? ctx->level_name : nullptr;
  ++seen.calls;
}

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char*) {
  g_init = Seen{};
  g_frame = Seen{};
  g_state = Seen{};
  Record(g_init, ctx);
  return 0;
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext* ctx) {
  Record(g_frame, ctx);
}

NEVR_PLUGIN_API void NvrPluginOnGameStateChange(const NvrGameContext* ctx, uint32_t oldState, uint32_t newState) {
  Record(g_state, ctx);
  if (!g_state.covered || ctx->plugin_id == nullptr || ctx->log == nullptr) return;
  char from[16];
  char to[16];
  char symbol[32];
  std::snprintf(from, sizeof(from), "%u", oldState);
  std::snprintf(to, sizeof(to), "%u", newState);
  std::snprintf(symbol, sizeof(symbol), "0x%016llx", static_cast<unsigned long long>(ctx->level_symbol));
  const NevrLogField fields[] = {
      {"from", from}, {"to", to}, {"level_symbol", symbol}, {"level", ctx->level_name != nullptr ? ctx->level_name : "?"}};
  ctx->log(ctx->plugin_id, NEVR_LOG_INFO, "state change level", fields, 4);
}

// Test-only observability: what each kind of context carried last (which: 0 init, 1 frame, 2 state change).
NEVR_PLUGIN_API uint32_t NevrTestPluginLevelCovered(uint32_t which) {
  const Seen& s = which == 0u ? g_init : which == 1u ? g_frame : g_state;
  return s.covered ? 1u : 0u;
}

NEVR_PLUGIN_API uint64_t NevrTestPluginLevelSymbol(uint32_t which) {
  return (which == 0u ? g_init : which == 1u ? g_frame : g_state).symbol;
}

NEVR_PLUGIN_API const char* NevrTestPluginLevelName(uint32_t which) {
  return (which == 0u ? g_init : which == 1u ? g_frame : g_state).name;
}

NEVR_PLUGIN_API uint32_t NevrTestPluginLevelCalls(uint32_t which) {
  return (which == 0u ? g_init : which == 1u ? g_frame : g_state).calls;
}
