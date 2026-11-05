// Fixture: a plugin built before the prefix rename, so it exports only the deprecated Nvr* entry points. The
// loader must still load it and call every one. The types are the Nevr* ones: the export names are what is old.
#include "nevr/plugin_interface.h"

#include <cstring>

namespace {

constexpr NevrPluginInfo kInfo = {
    "test-plugin-deprecated-exports",
    "Hermetic fixture: deprecated Nvr* exports only",
    1u,
    0u,
    0u,
};

uint32_t g_initExCalls = 0u;
uint32_t g_frames = 0u;
uint32_t g_stateChanges = 0u;
bool g_argsSeen = false;
uint32_t g_shutdowns = 0u;

}  // namespace

NEVR_PLUGIN_API NevrPluginInfo NvrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void) {
  return NEVR_PLUGIN_CAP_OBSERVES_ONLY;
}

NEVR_PLUGIN_API int NvrPluginInitEx(const NevrGameContext*, const char* args_json) {
  ++g_initExCalls;
  g_argsSeen = args_json != nullptr && std::strstr(args_json, "\"probe\":\"1\"") != nullptr;
  return 0;
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NevrGameContext*) {
  ++g_frames;
}

NEVR_PLUGIN_API void NvrPluginOnGameStateChange(const NevrGameContext*, uint32_t, uint32_t) {
  ++g_stateChanges;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
  ++g_shutdowns;
}

// Test-only observability export: init-with-args calls, frames, state changes, args seen (0/1), shutdowns,
// in that order for which = 0..4.
NEVR_PLUGIN_API uint32_t NvrTestPluginExportsGetCount(uint32_t which) {
  switch (which) {
    case 0: return g_initExCalls;
    case 1: return g_frames;
    case 2: return g_stateChanges;
    case 3: return g_argsSeen ? 1u : 0u;
    case 4: return g_shutdowns;
    default: return 0xFFFFFFFFu;
  }
}
