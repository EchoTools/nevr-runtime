// Peer timeout logging rules (see peer_timeout_log.h).

#include "runtime/compat/peer_timeout_log.h"

#include <cstddef>
#include <cstdio>

namespace nevr_peer_timeout_log {
namespace {

constexpr std::uint64_t kMicrosPerSecond = 1'000'000ULL;
constexpr std::uint64_t kMicrosPerMilli = 1'000ULL;
// Room for "%llu.%03llu": the largest 64-bit seconds value is 20 digits, plus the point and the milliseconds.
constexpr std::size_t kSecondsTextSize = 32;

// Microseconds as "S.mmm" seconds, integer arithmetic only.
std::string FormatSeconds(std::uint64_t micros) {
    char buffer[kSecondsTextSize];
    std::snprintf(buffer, sizeof(buffer), "%llu.%03llu",
                  static_cast<unsigned long long>(micros / kMicrosPerSecond),
                  static_cast<unsigned long long>((micros % kMicrosPerSecond) / kMicrosPerMilli));
    return buffer;
}

std::string FormatLastSend(const SendTracker& tracker) {
    if (!tracker.LastSend().has_value()) return "none";
    return std::to_string(*tracker.LastSend());
}

}  // namespace

namespace {

bool IsTimeoutReason(std::string_view reason) {
    return reason == "communication timeout" || reason == "connect timeout" || reason == "disconnect timeout" ||
           reason == "no connect received";
}

bool IsAcknowledgedReason(std::string_view reason) {
    return reason == "disconnect received" || reason == "disconnect acknowledged" || reason == "disconnected";
}

}  // namespace

bool RecognizedRemovalReason(std::string_view reason) { return IsTimeoutReason(reason) || IsAcknowledgedReason(reason); }

RemovalLevel LevelForRemovalReason(std::string_view reason) {
    // An unknown reason is a Warning: a reason this table has not named is one nobody has read yet.
    if (IsAcknowledgedReason(reason)) return RemovalLevel::kInfo;
    return RemovalLevel::kWarning;
}

std::string FormatPeerRemoved(const PeerRemoval& removal) {
    const std::uint64_t gap = removal.now_us > removal.last_receive_us ? removal.now_us - removal.last_receive_us : 0;
    std::string line = "[NEVR.GAMESERVER] peer removed reason=" + removal.reason + " slot=" + std::to_string(removal.slot) +
                       " address=" + removal.address + " last_receive_us=" + std::to_string(removal.last_receive_us) +
                       " gap_s=" + FormatSeconds(gap);
    if (!RecognizedRemovalReason(removal.reason)) line += " reason_recognized=no (the reason is unrecognized)";
    return line;
}

void SendTracker::EnterInGame(std::uint64_t now_us) {
    in_game_ = true;
    warned_ = false;
    quiet_since_us_ = now_us;
}

void SendTracker::LeaveInGame() { in_game_ = false; }

void SendTracker::RecordSend(std::uint64_t now_us) {
    ++sends_;
    has_send_ = true;
    last_send_us_ = now_us;
    quiet_since_us_ = now_us;
    warned_ = false;
}

std::optional<std::uint64_t> SendTracker::CheckNoSend(std::uint64_t now_us) {
    if (!in_game_ || warned_) return std::nullopt;
    const std::uint64_t silent = now_us > quiet_since_us_ ? now_us - quiet_since_us_ : 0;
    if (silent < kNoSendWarnMicros) return std::nullopt;
    warned_ = true;
    return silent;
}

std::string FormatInGameSendState(const SendTracker& tracker) {
    return "[NEVR.WS] in game send_state last_send_us=" + FormatLastSend(tracker) +
           " sends=" + std::to_string(tracker.Sends());
}

std::string FormatNoSendWarning(const SendTracker& tracker, std::uint64_t silent_us) {
    return "[NEVR.WS] no send to host for " + FormatSeconds(kNoSendWarnMicros) +
           " s silent_s=" + FormatSeconds(silent_us) +
           " last_send_us=" + FormatLastSend(tracker) + " sends=" + std::to_string(tracker.Sends());
}

}  // namespace nevr_peer_timeout_log
