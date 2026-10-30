// Port scan for the game's HTTP session API listener (issue #1511).
//
// The game's bind wrapper (fcn.1401F5B00, see binary_bug_fixes.cpp) returns 1 when it
// bound the port and 0 when it did not. On 0 it frees its own listener and leaves the
// caller's state untouched, so the same call can be made again on the next port.
// This header holds only the scan logic and its wording: no Win32 calls, so the
// in-use case is testable without a socket.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nevr_http_port_scan {

// Ports tried after the starting port the game asked for.
inline constexpr int kFallbackPorts = 8;

// WSAGetLastError() value for "address already in use" (WSAEADDRINUSE).
inline constexpr int kErrAddressInUse = 10048;

struct Attempt {
    uint16_t port = 0;
    uint64_t result = 0;  // the wrapper's return: 1 bound, 0 not bound
    int error = 0;        // WSAGetLastError() right after a 0 return; 0 after a bind
};

struct Scan {
    std::vector<Attempt> attempts;

    bool bound() const { return !attempts.empty() && attempts.back().result != 0; }
    uint16_t port() const { return attempts.empty() ? 0 : attempts.back().port; }
};

// Calls bind(port) for `start`, then start+1, ... up to kFallbackPorts more, stopping
// at the first call that returns nonzero or at port 65535. `bind` returns an Attempt
// whose error field the caller filled in right after the call.
template <typename BindFn>
Scan ScanPorts(uint16_t start, BindFn&& bind) {
    Scan scan;
    for (int i = 0; i <= kFallbackPorts; ++i) {
        const uint32_t candidate = static_cast<uint32_t>(start) + static_cast<uint32_t>(i);
        if (candidate > 0xFFFFu) break;
        scan.attempts.push_back(bind(static_cast<uint16_t>(candidate)));
        if (scan.bound()) break;
    }
    return scan;
}

// What a Win32 error from a failed bind means to a player's tool, in a few words.
inline std::string ErrorMeaning(int error) {
    if (error == 0) return "no error reported";
    if (error == kErrAddressInUse) return "address already in use";
    if (error == 10013) return "access denied";
    return "see WSAGetLastError";
}

// "6721 err=10048 (address already in use), 6722 err=10048 (address already in use)"
inline std::string DescribeAttempts(const std::vector<Attempt>& attempts) {
    std::string out;
    for (const Attempt& a : attempts) {
        if (!out.empty()) out += ", ";
        out += std::to_string(a.port) + " err=" + std::to_string(a.error) + " (" + ErrorMeaning(a.error) + ")";
    }
    return out;
}

}  // namespace nevr_http_port_scan
