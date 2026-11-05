#include <windows.h>

#include "nevr/plugin_interface.h"

namespace {

constexpr char kEventName[] = "Local\\NEVRTestPartialInitShutdownObserved";
HANDLE g_observer = nullptr;

constexpr NevrPluginInfo kInfo = {
    "test-plugin-partial-init-fail",
    "Hermetic plugin-loader fixture that needs shutdown after failed init",
    1u,
    0u,
    0u,
};

}  // namespace

NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo(void) { return kInfo; }

NEVR_PLUGIN_API uint32_t NevrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

NEVR_PLUGIN_API int NevrPluginInit(const NevrGameContext*) {
  g_observer = OpenEventA(EVENT_MODIFY_STATE, FALSE, kEventName);
  return 1;
}

NEVR_PLUGIN_API void NevrPluginShutdown(void) {
  if (g_observer) {
    SetEvent(g_observer);
    CloseHandle(g_observer);
    g_observer = nullptr;
  }
}
