#include "nevr/plugin_interface.h"

namespace {

constexpr NevrPluginInfo kInfo = {
    "test-plugin-future-api",
    "Hermetic plugin-loader API mismatch test fixture",
    1u,
    0u,
    0u,
};

}  // namespace

NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NevrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION + 1u;
}
