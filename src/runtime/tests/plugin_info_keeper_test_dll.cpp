#include "nevr/plugin_interface.h"

namespace {

constexpr NevrPluginInfo kInfo = {
    "test-plugin-info-keeper",
    "Hermetic fixture: keeps what get_plugin_info returned during its init",
    1u,
    0u,
    0u,
};

const NevrLoadedPluginInfo* g_keptFirstPlugin = nullptr;

}  // namespace

NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NevrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION;
}

// Keeps the pointer for the plugin loaded before this one. The loader pushes this plugin into its
// list after this returns; that push must not move the entry the pointer names (#152).
NEVR_PLUGIN_API int NevrPluginInit(const NevrGameContext* ctx) {
  if (ctx->get_plugin_count != nullptr && ctx->get_plugin_info != nullptr && ctx->get_plugin_count() > 0) {
    g_keptFirstPlugin = ctx->get_plugin_info(0);
  }
  return 0;
}

// Test-only observability export; the loader never resolves it.
NEVR_PLUGIN_API const NevrLoadedPluginInfo* NevrTestPluginGetKeptInfo(void) {
  return g_keptFirstPlugin;
}
