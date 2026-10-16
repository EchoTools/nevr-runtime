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

std::string ConfigRejectionDetail(const NevrConfigError& error) {
  return std::string("reason=") + ReasonToken(error.Kind()) + " line=" + std::to_string(error.Line()) +
         " column=" + std::to_string(error.Column());
}

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
    const std::string detail = ConfigRejectionDetail(e);
    if (is_server) {
      // In a real server the registered handler routes this to ForceFatalExit
      // (the N4 path); the unregistered fallback message-boxes + exits. The
      // unit test captures this message with a registered handler.
      FatalError(("[NEVR.CONFIG] config.yaml rejected path=" + path + " " + detail).c_str(), "NEVR Config Error");
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.CONFIG] config.yaml rejected path=%s %s — continuing with empty config (all config-driven "
          "behavior uses defaults)",
          path.c_str(), detail.c_str());
    }
    return NevrConfig();  // empty
  }
}

}  // namespace nevr
