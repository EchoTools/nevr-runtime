// #598 fixture: a plugin that logs through the host (ctx->log) from init, per-frame, state change and
// shutdown. Built twice (TEST_PLUGIN_NAME differs) so a test can tell two plugins' identities apart.

#include "extension/plugin_interface.h"

#include <cstddef>
#include <cstdio>

#ifndef TEST_PLUGIN_NAME
#error "TEST_PLUGIN_NAME must name this build of the fixture"
#endif

namespace {

constexpr NvrPluginInfo kInfo = {
    TEST_PLUGIN_NAME,
    "Hermetic plugin-loader fixture that logs through the host",
    1u,
    0u,
    0u,
};

// What a plugin keeps from its init context so it can log from Shutdown, which gets no context.
const char* g_id = nullptr;
NevrLogFn g_log = nullptr;
uint32_t g_frames = 0u;
bool g_boundAtInit = false;

bool CtxCoversLog(const NvrGameContext* ctx) {
  return ctx != nullptr && ctx->ctx_size >= offsetof(NvrGameContext, log) + sizeof(NevrLogFn) &&
         ctx->plugin_id != nullptr && ctx->log != nullptr;
}

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char*) {
  g_frames = 0u;
  g_boundAtInit = CtxCoversLog(ctx);
  if (!g_boundAtInit) return 0;
  g_id = ctx->plugin_id;
  g_log = ctx->log;
  const NevrLogField fields[] = {{"answer", "42"}, {"who", "a b"}};
  g_log(g_id, NEVR_LOG_WARNING, "init hello", fields, 2);
  return 0;
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext* ctx) {
  ++g_frames;
  if (!CtxCoversLog(ctx)) return;
  char n[16];
  std::snprintf(n, sizeof(n), "%u", g_frames);
  const NevrLogField fields[] = {{"n", n}};
  ctx->log(ctx->plugin_id, NEVR_LOG_DEBUG, "frame", fields, 1);
}

NEVR_PLUGIN_API void NvrPluginOnGameStateChange(const NvrGameContext* ctx, uint32_t oldState, uint32_t newState) {
  if (!CtxCoversLog(ctx)) return;
  char from[16];
  char to[16];
  std::snprintf(from, sizeof(from), "%u", oldState);
  std::snprintf(to, sizeof(to), "%u", newState);
  const NevrLogField fields[] = {{"from", from}, {"to", to}};
  ctx->log(ctx->plugin_id, NEVR_LOG_INFO, "state change", fields, 2);
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
  if (g_log != nullptr) g_log(g_id, NEVR_LOG_ERROR, "shutdown bye", nullptr, 0);
}

// Test-only observability: did init see a context that covers ctx->log?
NEVR_PLUGIN_API uint32_t NevrTestPluginGetLogBoundAtInit(void) {
  return g_boundAtInit ? 1u : 0u;
}
