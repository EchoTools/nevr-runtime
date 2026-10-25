#pragma once

// The one boot-log line that says where the runtime is running (#673): the Wine version, the host OS Wine
// reports, and the Windows build number. Pure, so it is unit-tested without the game. A host that cannot be
// measured prints `none` / `unknown`, never a guess: the same rule as nevr_system_info::Host.

#include <string>

#include "core/system_info.h"

namespace nevr_host_environment {

inline std::string BootLine(const nevr_system_info::Host& host) {
  std::string line = "[NEVR.BOOT] host environment: wine=";
  line += host.wine_version.empty() ? "none" : host.wine_version;
  line += " host_os=";
  line += host.wine_host_os.empty() ? "none" : host.wine_host_os;
  line += " os_build=";
  line += host.os_build == 0 ? std::string("unknown") : std::to_string(host.os_build);
  line += "\n";
  return line;
}

}  // namespace nevr_host_environment
