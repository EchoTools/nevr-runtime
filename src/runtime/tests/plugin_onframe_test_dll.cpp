#include "nevr/plugin_interface.h"

#include <windows.h>

namespace {

constexpr NevrPluginInfo kInfo = {
    "test-plugin-onframe",
    "Hermetic plugin-loader OnFrame dispatch test fixture",
    1u,
    0u,
    0u,
};

uint32_t g_frameCount = 0u;
uint32_t g_initCount = 0u;

}  // namespace

NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NevrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NevrPluginInit(const NevrGameContext*) {
  ++g_initCount;
  g_frameCount = 0u;
  return 0;
}

NEVR_PLUGIN_API void NevrPluginOnFrame(const NevrGameContext*) {
  ++g_frameCount;
}

// Test-only observability export. The loader never resolves this symbol; the
// behavioral test uses it after real LoadPlugins/TickPlugins dispatch to prove
// the callback running came from this DLL rather than an injected test hook.
NEVR_PLUGIN_API uint32_t NevrTestPluginGetFrameCount(void) {
  return g_frameCount;
}

// Test-only observability export: how many times the loader ran this DLL's init.
NEVR_PLUGIN_API uint32_t NevrTestPluginGetInitCount(void) {
  return g_initCount;
}

NEVR_PLUGIN_API void NevrPluginShutdown(void) {
  HANDLE shutdownObserved = OpenEventA(EVENT_MODIFY_STATE, FALSE,
                                      "Local\\NEVRTestPluginShutdownObserved");
  if (shutdownObserved != nullptr) {
    SetEvent(shutdownObserved);
    CloseHandle(shutdownObserved);
  }
}
