#include "nevr/plugin_interface.h"

namespace {

constexpr NevrPluginInfo kInfo = {
    "test-plugin-init-fail",
    "Hermetic plugin-loader fixture whose init reports failure",
    1u,
    0u,
    0u,
};

}  // namespace

NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NevrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NevrPluginInit(const NevrGameContext*) {
  return 1;
}
