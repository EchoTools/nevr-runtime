#include <windows.h>

#include "nevr/plugin_interface.h"

namespace {

constexpr char kEventName[] = "Local\\NEVRTestPartialInitShutdownObserved";
HANDLE g_observer = nullptr;

constexpr NvrPluginInfo kInfo = {
    "test-plugin-partial-init-fail",
    "Hermetic plugin-loader fixture that needs shutdown after failed init",
    1u,
    0u,
    0u,
};

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) { return kInfo; }

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext*) {
  g_observer = OpenEventA(EVENT_MODIFY_STATE, FALSE, kEventName);
  return 1;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
  if (g_observer) {
    SetEvent(g_observer);
    CloseHandle(g_observer);
    g_observer = nullptr;
  }
}
