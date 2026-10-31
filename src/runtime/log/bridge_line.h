#pragma once

// #1294: the game reports its own connection to the local bridge at the default level (for example
// `[EVR] [TCP CLIENT] [R14NETCLIENT] connection to ws://127.0.0.1:<port>...`). The bridge is this
// runtime's own loopback listener, so these lines carry no operator signal at the default level.
// The built-in filter demotes a game line that names the loopback address to Debug; it does not touch
// a line that names any other address, and it never touches a line the runtime writes (N77).

#include <cstddef>
#include <cstring>

namespace nevr_bridge_line {

// The loopback address the bridge listens on (ws_bridge.cpp, WebSocketServer(port, "127.0.0.1")).
inline constexpr const char kBridgeAddress[] = "127.0.0.1";

// True when `message` is a game line (no `[NEVR.` tag) that names the bridge address as a whole
// address: "127.0.0.1" followed by a non-digit, non-dot character (or the end of the line), and not
// preceded by a digit or dot, so "127.0.0.10" and "1127.0.0.1" do not match.
inline bool IsBridgeConnectionLine(const char* message) {
    if (message == nullptr) return false;
    if (std::strncmp(message, "[NEVR.", 6) == 0) return false;

    const std::size_t addrLen = sizeof(kBridgeAddress) - 1;
    const char* at = message;
    while ((at = std::strstr(at, kBridgeAddress)) != nullptr) {
        const char before = at == message ? '\0' : at[-1];
        const char after = at[addrLen];
        const bool startOk = !(before >= '0' && before <= '9') && before != '.';
        const bool endOk = !(after >= '0' && after <= '9') && after != '.';
        if (startOk && endOk) return true;
        at += addrLen;
    }
    return false;
}

}  // namespace nevr_bridge_line
