#include "extension/plugin_interface.h"

namespace {

constexpr NvrPluginInfo kInfo = {
    "test-plugin-onframe",
    "Hermetic plugin-loader OnFrame dispatch test fixture",
    1u,
    0u,
    0u,
};

uint32_t g_frameCount = 0u;

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext*) {
  g_frameCount = 0u;
  return 0;
}

NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext*) {
  ++g_frameCount;
}

// Test-only observability export. The loader never resolves this symbol; the
// behavioral test uses it after real LoadPlugins/TickPlugins dispatch to prove
// the callback running came from this DLL rather than an injected test hook.
NEVR_PLUGIN_API uint32_t NvrTestPluginGetFrameCount(void) {
  return g_frameCount;
}
