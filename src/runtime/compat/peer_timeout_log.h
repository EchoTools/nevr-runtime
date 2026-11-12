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
// tested with fixed numbers (test_peer_timeout_log.cpp). The per-binary hooks live in patch/peer_timeout_log_hook.h.

#pragma once

#include <cstdint>
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
RemovalLevel LevelForRemovalReason(std::string_view reason);
bool RecognizedRemovalReason(std::string_view reason);

// One peer the game removed (CBroadcaster::DestroyPeer, the reason the caller passes).
struct PeerRemoval {
    std::uint64_t slot;             // the peer's index in the broadcaster's peer table
    std::string reason;             // the game's reason text (the caller's string)
    std::string address;            // the peer's address text, as the game prints it (peer record +0x1a0)
    std::uint64_t last_receive_us;  // the game's last-received stamp for the peer (peer record +0x228)
    std::uint64_t now_us;           // the broadcaster's clock at the removal (broadcaster record +0x478)
};

// "[NEVR.GAMESERVER] peer removed reason=.. slot=.. address=.. last_receive_us=.. gap_s=S.mmm"
// The gap is now_us minus last_receive_us, clamped at 0 when the clock reads earlier than the stamp.
std::string FormatPeerRemoved(const PeerRemoval& removal);

// Tracks the client's sends to its host while it is in game.
class SendTracker {
public:
    // Entering in-game starts the silence clock at now_us and re-arms the no-send warning.
    void EnterInGame(std::uint64_t now_us);
    // Leaving in-game stops the no-send check; the send count is kept.
    void LeaveInGame();
    // One send to the host at now_us. Re-arms the no-send warning.
    void RecordSend(std::uint64_t now_us);
    // Returns the silence in microseconds the first time the client has been silent for kNoSendWarnMicros since its
    // last send (or since entering in-game), and nullopt otherwise. Not in game: nullopt.
    std::optional<std::uint64_t> CheckNoSend(std::uint64_t now_us);

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

// "[NEVR.WS] in game send_state last_send_us=<n|none> sends=<n>"
std::string FormatInGameSendState(const SendTracker& tracker);

// "[NEVR.WS] no send to host for 5.000 s silent_s=S.mmm last_send_us=<n|none> sends=<n>"
std::string FormatNoSendWarning(const SendTracker& tracker, std::uint64_t silent_us);

}  // namespace nevr_peer_timeout_log
