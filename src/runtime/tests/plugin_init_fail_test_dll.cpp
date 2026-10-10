#include "extension/plugin_interface.h"

namespace {

constexpr NvrPluginInfo kInfo = {
    "test-plugin-init-fail",
    "Hermetic plugin-loader fixture whose init reports failure",
    1u,
    0u,
    0u,
};

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext*) {
  return 1;
}
