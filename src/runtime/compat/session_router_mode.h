#pragma once
// Which game sockets the PC bridge hands to nevr_session_router (#708): config.yaml `network.session_router`,
// flat key nevr_session_router. Pure parsing, no logging and no globals, so a test can drive every value.
//
//   legacy  every game socket takes the bridge's own routing rules (the default until #708 S3)
//   config  the /config sockets route through the router; login and matchmaker stay on the bridge's rules
//   all     every game socket routes through the router (#708 S2; not available in this build)

#include <string>

#include "runtime/compat/socket_role.h"

namespace nevr_session_router_mode {

enum class Mode { Legacy, Config, All };

inline const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::Legacy: return "legacy";
    case Mode::Config: return "config";
    case Mode::All: return "all";
  }
  return "legacy";
}

// True when `mode` hands a socket of role `role` to the router. S1: config sockets only.
inline bool RoutesRole(Mode mode, nevr_socket_role::Role role) {
  return mode != Mode::Legacy && role == nevr_socket_role::Role::Config;
}

// The mode `value` names. A null or empty value is Legacy with no problem (the key is absent). A value that names
// no mode, and `all` while the build cannot run it, is Legacy with *problem set to a sentence for the log: the
// bridge never guesses a router mode from a misspelling.
inline Mode ParseMode(const char* value, std::string* problem) {
  if (problem != nullptr) problem->clear();
  if (value == nullptr || value[0] == '\0') return Mode::Legacy;
  const std::string text(value);
  if (text == "legacy") return Mode::Legacy;
  if (text == "config") return Mode::Config;
  if (problem != nullptr) {
    if (text == "all") {
      *problem = "\"all\" is not available in this build (login and matchmaker sockets still use the bridge's own rules)";
    } else {
      *problem = "\"" + text.substr(0, 32) + "\" is not legacy, config or all";
    }
  }
  return Mode::Legacy;
}

}  // namespace nevr_session_router_mode
