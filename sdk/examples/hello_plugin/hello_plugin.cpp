// hello_plugin: the smallest plugin that loads. It reports its metadata, binds to the host's log in init and says
// so through it. The same file builds for Windows and Quest; nothing here includes <windows.h> or a file of the
// runtime's tree, only the installed SDK headers (nevr/plugin_interface.h, nevr/plugin_logger.h).
#include "nevr/plugin_interface.h"
#include "nevr/plugin_logger.h"

namespace {
constexpr const char* kPluginName = "hello_plugin";
constexpr const char* kPluginDescription = "Example plugin built against the installed plugin SDK";
constexpr uint32_t kVersionMajor = 1;
constexpr uint32_t kVersionMinor = 0;
constexpr uint32_t kVersionPatch = 0;
constexpr const char* kLogPrefix = "[hello_plugin]";
}  // namespace

NEVR_DEFINE_PLUGIN_LOG(kLogPrefix)

NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo() {
    return NevrPluginInfo{kPluginName, kPluginDescription, kVersionMajor, kVersionMinor, kVersionPatch};
}

NEVR_PLUGIN_API int NevrPluginInit(const NevrGameContext* ctx) {
    PluginLogBind(ctx);
    const std::string apiVersion = std::to_string(NEVR_PLUGIN_API_VERSION);
    PluginLogFields(NEVR_LOG_INFO, "initialized", {{"api_version", apiVersion.c_str()}});
    return 0;
}

NEVR_PLUGIN_API void NevrPluginShutdown() {
    PluginLog("shutdown");
}
