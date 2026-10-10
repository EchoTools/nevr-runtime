// Portable redirect policy shared by PCVR and Quest builds.
#include "runtime/lifecycle/service_redirect.h"

namespace nevr_cfg {
namespace {

bool StartsWith(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

}  // namespace

std::optional<std::string> ResolveRedirect(const std::string& result,
                                           const std::optional<std::string>& socketTarget,
                                           const std::optional<std::string>& httpTarget,
                                           bool bridgeActive, unsigned bridgePort) {
  const bool isWebSocket = StartsWith(result, "wss://") || StartsWith(result, "ws://");
  const bool isReadyAtDawn = result.find("readyatdawn.com") != std::string::npos;

  // Only ws/wss URLs (any host) or https readyatdawn.com URLs are redirected;
  // everything else passes through unchanged.
  if (!isWebSocket && !isReadyAtDawn) return std::nullopt;

  const std::optional<std::string>& target = isWebSocket ? socketTarget : httpTarget;
  if (!target || target->empty()) return std::nullopt;

  // ws/wss with the bridge up route through the in-process relay. https never
  // hits the bridge because the game's native TLS can reach the raw HTTP target.
  if (bridgeActive && isWebSocket) {
    return std::string("ws://127.0.0.1:") + std::to_string(bridgePort);
  }
  return *target;
}

}  // namespace nevr_cfg
