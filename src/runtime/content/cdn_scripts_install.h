// The PC's script registrar for the CDN (cdn/cdn_scripts.h; docs/adr/0045 decision 4; PLAN-v5 5.3, R10b). Its claims are the script
// provider's (runtime/content/script_provider.h) under the owner "cargo:<label>". cdn_start.cpp sets it on the client's CDN config.
#pragma once

#include <string>

#include "cdn/cdn_script_gate.h"

namespace nevr_cdn_scripts {

// The registrar for the CDN cache folder `cache_dir` (`<nevr root>\cache\cdn`, no trailing separator): the copies are written below it.
// `signed_only` is the run's -require-signed-plugins mode (ext/plugin_trust.h ActiveMode), the policy fact the provider needs.
nevr_cdn::ScriptRegistrar PcRegistrar(const std::string& cache_dir, bool signed_only);

}  // namespace nevr_cdn_scripts
