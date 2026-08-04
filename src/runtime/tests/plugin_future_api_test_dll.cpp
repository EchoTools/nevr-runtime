#include "extension/plugin_interface.h"

namespace {

constexpr NvrPluginInfo kInfo = {
    "test-plugin-future-api",
    "Hermetic plugin-loader API mismatch test fixture",
    1u,
    0u,
    0u,
};

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return kInfo;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
  return NEVR_PLUGIN_API_VERSION + 1u;
}
