// A no-op plugin for the Quest loader's host test (src/quest/tests/plugin_loader_test.cpp). One source, built several
// times with different macros into lib<variant>.so: each variant is one way a plugin can be right or wrong. It records what
// it saw in `fx_seen`, which the test reads with dlsym; it logs through ctx->log the way a real plugin does.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "extension/plugin_interface.h"
#include "quest/tests/plugin_fixture_seen.h"

#ifndef FX_NAME
#define FX_NAME "fx"
#endif
// The build IDs this plugin says it supports (the test's fake running build is the second one).
#ifndef FX_IDS
#define FX_IDS "1111111111111111111111111111111111111111 2222222222222222222222222222222222222222"
#endif
#ifndef FX_INIT_RC
#define FX_INIT_RC 0
#endif

extern "C" {
__attribute__((visibility("default"))) FxSeen fx_seen = {};
}

namespace {
struct Ctor {
  Ctor() { fx_seen.ctor_ran = 1; }
} g_ctor;

void Copy(char* to, std::size_t cap, const char* from) {
  std::strncpy(to, from != nullptr ? from : "", cap - 1);
  to[cap - 1] = '\0';
}

void Record(const NvrGameContext* ctx, const char* args) {
  ++fx_seen.init_count;
  fx_seen.ctx_size = ctx->ctx_size;
  fx_seen.flags = ctx->flags;
  fx_seen.base_addr = ctx->base_addr;
  fx_seen.net_game = ctx->net_game;
  fx_seen.game_state = ctx->game_state;
  Copy(fx_seen.plugin_id, sizeof(fx_seen.plugin_id), ctx->plugin_id);
  Copy(fx_seen.args, sizeof(fx_seen.args), args);
  if (ctx->ctx_size >= offsetof(NvrGameContext, level_name) + sizeof(ctx->level_name)) {
    fx_seen.level_symbol = ctx->level_symbol;
    fx_seen.level_name = ctx->level_name;
  }
  fx_seen.covers_game_build_id =
      ctx->ctx_size >= offsetof(NvrGameContext, game_build_id) + sizeof(ctx->game_build_id) ? 1 : 0;
  if (fx_seen.covers_game_build_id != 0) Copy(fx_seen.game_build_id, sizeof(fx_seen.game_build_id), ctx->game_build_id);
  fx_seen.plugin_count_at_init = ctx->get_plugin_count != nullptr ? ctx->get_plugin_count() : -1;
  if (fx_seen.plugin_count_at_init > 0) {
    const NvrLoadedPluginInfo* first = ctx->get_plugin_info(0);
    Copy(fx_seen.first_plugin_name, sizeof(fx_seen.first_plugin_name), first != nullptr ? first->name : "");
  }
  const NevrLogField fields[] = {{"k", "v"}};
  ctx->log(ctx->plugin_id, NEVR_LOG_INFO, "fixture init", fields, 1);
}
}  // namespace

#ifndef FX_NO_INFO
NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) { return {FX_NAME, "loader test fixture", 1, 2, 3}; }
#endif
NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void) { return NEVR_PLUGIN_CAP_OBSERVES_ONLY; }
#ifndef FX_NO_IDS
NEVR_PLUGIN_API const char* NEVR_GetSupportedBuildIds(void) { return FX_IDS; }
#endif
#ifdef FX_LEGACY_INIT
NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
  Record(ctx, "");
  return FX_INIT_RC;
}
#else
NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char* args_json) {
  Record(ctx, args_json);
  return FX_INIT_RC;
}
#endif
NEVR_PLUGIN_API void NvrPluginShutdown(void) { ++fx_seen.shutdown_count; }
