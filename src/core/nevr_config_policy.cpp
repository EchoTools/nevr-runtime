// nevr_config_policy.cpp — the PC-only half of nevr_config (N4, N133): what the runtime does when a config
// fails to load, and the unset-variable warning. It logs through core/logging.h, which the Quest does not
// build, so nevr_config.cpp (the parser, shared with the Quest) stays free of it.
//
// Compiled WITHOUT the core PCH (SKIP_PRECOMPILE_HEADERS in CMake) and with NOMINMAX before windows.h, like
// nevr_config.cpp: the PCH includes <windows.h> without NOMINMAX, whose min/max macros break <optional>/<limits>.

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>  // CHAR / VOID typedefs used by core/logging.h
#endif

#include "core/nevr_config.h"

#include "core/logging.h"  // Log(), FatalError()

namespace nevr {

void LogUnsetBareVars(const NevrConfig& cfg) {
  for (const std::string& name : cfg.UnsetBareVars()) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.CONFIG] environment variable %s is not set; the text ${%s} is kept as written, and a "
        "service key that uses it counts as unset",
        name.c_str(), name.c_str());
  }
}

NevrConfig NevrConfig::LoadFromFileOrFail(const std::string& path, bool is_server) {
  try {
    NevrConfig cfg = LoadFromFile(path);
    LogUnsetBareVars(cfg);
    return cfg;
  } catch (const NevrConfigError& e) {
    if (is_server) {
      // In a real server the registered handler routes this to ForceFatalExit
      // (the N4 path); the unregistered fallback message-boxes + exits. Never
      // reached by the unit test, which drives the client branch only.
      FatalError(e.what(), "NEVR Config Error");
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.CONFIG] %s — continuing with empty config (all config-driven "
          "behavior uses defaults)",
          e.what());
    }
    return NevrConfig();  // empty
  }
}

}  // namespace nevr
