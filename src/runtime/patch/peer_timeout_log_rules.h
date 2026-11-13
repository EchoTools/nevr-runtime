// Peer timeout logging (server and client), one rule for PC and Quest.
//
// Server: when the game removes a peer for the communication timeout (CleanupPeers' removal branch; the game's own
// text "(communication timeout)" comes only from that branch), one Warning line names the peer, its slot, the time the
// game last heard from it and the gap. The last-receive stamp is the game's own: the peer record keeps it at +0x228
// (microseconds, the broadcaster's clock; the same clock as the broadcaster's "now" at +0x478), so nothing new is
// written per datagram.
//
// Client: entering in-game logs one Info line with the client's send state; a client that has sent nothing to its host
// for kNoSendWarnMicros logs one Warning, once per silence.
//
// This file is pure: no Windows call, no game memory, no clock read. The caller passes every time in, so the rules are
// tested with fixed numbers (peer_timeout_log_rules_test.cpp). The platform halves are patch/peer_timeout_log.h (PC)
// and src/quest/integration/peer_timeout_log.h (Quest).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace nevr_peer_timeout_log {

// A log threshold, not a game limit: how long a client may go without sending to its host before one Warning is
// written. The game's own communication timeout (the broadcaster's +0x488, 15 s) is read from the game, not named here.
inline constexpr std::uint64_t kNoSendWarnMicros = 5'000'000ULL;

// How the log writes a removal: the reason's own level. A timeout is a Warning (the peer was silent past a limit); a
// disconnect the peer or the game acknowledged is Info. An unknown reason is a Warning, and its line says it was
// unrecognized (FormatPeerRemoved).
enum class RemovalLevel { kWarning, kInfo };

// One peer the game removed (CBroadcaster::DestroyPeer, the reason the caller passes).
struct PeerRemoval {
    std::uint64_t slot;             // the peer's index in the broadcaster's peer table
    std::string reason;             // the game's reason text (the caller's string)
    std::string address;            // the peer's address text, as the game prints it (peer record +0x1a0)
    std::uint64_t last_receive_us;  // the game's last-received stamp for the peer (peer record +0x228)
    std::uint64_t now_us;           // the broadcaster's clock at the removal (broadcaster record +0x478)
};

// Tracks the client's sends to its host while it is in game.
class SendTracker {
public:
    // Entering in-game starts the silence clock at now_us and re-arms the no-send warning.
    void EnterInGame(std::uint64_t now_us) {
        in_game_ = true;
        warned_ = false;
        quiet_since_us_ = now_us;
    }
    // Leaving in-game stops the no-send check; the send count is kept.
    void LeaveInGame() { in_game_ = false; }
    // One send to the host at now_us. Re-arms the no-send warning.
    void RecordSend(std::uint64_t now_us) {
        ++sends_;
        has_send_ = true;
        last_send_us_ = now_us;
        quiet_since_us_ = now_us;
        warned_ = false;
    }
    // Returns the silence in microseconds the first time the client has been silent for kNoSendWarnMicros since its
    // last send (or since entering in-game), and nullopt otherwise. Not in game: nullopt.
    std::optional<std::uint64_t> CheckNoSend(std::uint64_t now_us) {
        if (!in_game_ || warned_) return std::nullopt;
        const std::uint64_t silent = now_us > quiet_since_us_ ? now_us - quiet_since_us_ : 0;
        if (silent < kNoSendWarnMicros) return std::nullopt;
        warned_ = true;
        return silent;
    }

    bool InGame() const { return in_game_; }
    std::uint64_t Sends() const { return sends_; }
    std::optional<std::uint64_t> LastSend() const {
        return has_send_ ? std::optional<std::uint64_t>(last_send_us_) : std::nullopt;
    }

private:
    bool in_game_ = false;
    bool warned_ = false;
    std::uint64_t sends_ = 0;
    std::uint64_t last_send_us_ = 0;
    bool has_send_ = false;
    std::uint64_t quiet_since_us_ = 0;
};

namespace detail {

inline constexpr std::uint64_t kMicrosPerSecond = 1'000'000ULL;
inline constexpr std::uint64_t kMicrosPerMilli = 1'000ULL;
// Room for "%llu.%03llu": the largest 64-bit seconds value is 20 digits, plus the point and the milliseconds.
inline constexpr std::size_t kSecondsTextSize = 32;

// Microseconds as "S.mmm" seconds, integer arithmetic only.
inline std::string FormatSeconds(std::uint64_t micros) {
    char buffer[kSecondsTextSize];
    std::snprintf(buffer, sizeof(buffer), "%llu.%03llu",
                  static_cast<unsigned long long>(micros / kMicrosPerSecond),
                  static_cast<unsigned long long>((micros % kMicrosPerSecond) / kMicrosPerMilli));
    return buffer;
}

inline std::string FormatLastSend(const SendTracker& tracker) {
    if (!tracker.LastSend().has_value()) return "none";
    return std::to_string(*tracker.LastSend());
}

inline bool IsTimeoutReason(std::string_view reason) {
    return reason == "communication timeout" || reason == "connect timeout" || reason == "disconnect timeout" ||
           reason == "no connect received";
}

inline bool IsAcknowledgedReason(std::string_view reason) {
    return reason == "disconnect received" || reason == "disconnect acknowledged" || reason == "disconnected";
}

}  // namespace detail

inline bool RecognizedRemovalReason(std::string_view reason) {
    return detail::IsTimeoutReason(reason) || detail::IsAcknowledgedReason(reason);
}

inline RemovalLevel LevelForRemovalReason(std::string_view reason) {
    // An unknown reason is a Warning: a reason this table has not named is one nobody has read yet.
    if (detail::IsAcknowledgedReason(reason)) return RemovalLevel::kInfo;
    return RemovalLevel::kWarning;
}

// "[NEVR.GAMESERVER] peer removed reason=.. slot=.. address=.. last_receive_us=.. gap_s=S.mmm"
// The gap is now_us minus last_receive_us, clamped at 0 when the clock reads earlier than the stamp.
inline std::string FormatPeerRemoved(const PeerRemoval& removal) {
    const std::uint64_t gap = removal.now_us > removal.last_receive_us ? removal.now_us - removal.last_receive_us : 0;
    std::string line = "[NEVR.GAMESERVER] peer removed reason=" + removal.reason + " slot=" + std::to_string(removal.slot) +
                       " address=" + removal.address + " last_receive_us=" + std::to_string(removal.last_receive_us) +
                       " gap_s=" + detail::FormatSeconds(gap);
    if (!RecognizedRemovalReason(removal.reason)) line += " reason_recognized=no (the reason is unrecognized)";
    return line;
}

// "[NEVR.WS] in game send_state last_send_us=<n|none> sends=<n>"
inline std::string FormatInGameSendState(const SendTracker& tracker) {
    return "[NEVR.WS] in game send_state last_send_us=" + detail::FormatLastSend(tracker) +
           " sends=" + std::to_string(tracker.Sends());
}

// "[NEVR.WS] no send to host for 5.000 s silent_s=S.mmm last_send_us=<n|none> sends=<n>"
inline std::string FormatNoSendWarning(const SendTracker& tracker, std::uint64_t silent_us) {
    return "[NEVR.WS] no send to host for " + detail::FormatSeconds(kNoSendWarnMicros) +
           " s silent_s=" + detail::FormatSeconds(silent_us) +
           " last_send_us=" + detail::FormatLastSend(tracker) + " sends=" + std::to_string(tracker.Sends());
}

}  // namespace nevr_peer_timeout_log
