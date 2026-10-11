// Pure service URL redirect decision shared by the Windows runtime and Quest.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace nevr_cfg {

/// Decide whether the game's URL should be replaced. A missing result leaves
/// the game's original pointer/value untouched.
///
/// `socketTarget` applies to ws/wss URLs. `httpTarget` applies only to
/// readyatdawn.com URLs; https redirects never use the local WebSocket bridge.
/// When the bridge is active, eligible ws/wss URLs resolve to loopback, followed by `bridgePath` (the PCVR
/// bridge's role path for the service, #513; empty for Quest).
std::optional<std::string> ResolveRedirect(const std::string& result,
                                           const std::optional<std::string>& socketTarget,
                                           const std::optional<std::string>& httpTarget,
                                           bool bridgeActive, unsigned bridgePort,
                                           std::string_view bridgePath = {});

}  // namespace nevr_cfg
