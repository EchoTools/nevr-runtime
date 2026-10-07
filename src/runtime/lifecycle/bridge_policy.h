#pragma once

// What boot does when no services.socket_uri is configured (#16). The login WebSocket bridge is what
// injects the LoginRequest; a dedicated server without it connects straight to the dead
// readyatdawn.com hosts, never logs in, and idles forever. A fleet manager can't see that, so a server
// without a bridge refuses to start unless the operator says an offline boot is intended
// (services.allow_offline_server: true, used by the offline Windows-VM boot rig).
//
// Pure: no logging, no globals. The caller logs and acts on the outcome.

#include <string_view>

namespace BridgePolicy {

enum class Outcome {
  Start,              // socket_uri configured: start the bridge
  SkipClient,         // client without a socket_uri: the game talks to services directly
  SkipOfflineServer,  // server, no socket_uri, operator opted in to an offline boot
  RefuseServer,       // server, no socket_uri, no opt-in: fatal
};

constexpr Outcome Decide(bool hasSocketUri, bool isServer, bool allowOfflineServer) {
  if (hasSocketUri) return Outcome::Start;
  if (!isServer) return Outcome::SkipClient;
  return allowOfflineServer ? Outcome::SkipOfflineServer : Outcome::RefuseServer;
}

// config.yaml scalars arrive as strings; only an explicit truthy value opts in.
inline bool IsTruthy(const char* value) {
  if (value == nullptr) return false;
  const std::string_view v(value);
  return v == "true" || v == "True" || v == "TRUE" || v == "yes" || v == "on" || v == "1";
}

}  // namespace BridgePolicy
