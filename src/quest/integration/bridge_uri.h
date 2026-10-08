// The redirect value for the loopback bridge.
//
// The shared redirect policy (nevr_cfg::ResolveRedirect) answers the bare "ws://127.0.0.1:<port>" for a
// redirected game URL; the listener answers that with 403 because every upgrade must carry the per-start
// access token (quest_net::LoopbackGameServer::LoopbackUri, "ws://127.0.0.1:<port>/<token>/"). The sentinel
// interns the tokened URI in its place; any other value is interned as it is.
#pragma once

#include <string>
#include <string_view>

namespace nevr_quest::integration {

// `uri` when `value` is exactly "ws://127.0.0.1:<port>" and `uri` is non-empty; otherwise `value`.
std::string_view ReplaceBareBridgeUri(std::string_view value, unsigned port, const std::string& uri) noexcept;

}  // namespace nevr_quest::integration
