// Feeds the NEVR service's social messages into the models the Quest facade reads.
//
// The game's login connection carries EVR frames: marker(8) + symbol(8) + payload length(8) +
// payload, possibly several per transport message. The network adapter that owns the loopback
// bridge passes every frame it relays, in both directions, to ObserveFrames. The server-to-game
// ones update the friend roster, the recently-met list, display names and the party; the
// facade's slots read the result. Dispatch is by symbol hash: this binary has no symbol table to
// ask for names, and the hashes below are the same CSymbol64 values the Windows bridge matches
// by name.
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/social/social_facade.h"

namespace quest_social {

enum class Direction : std::uint8_t { kServerToGame, kGameToServer };

// CSymbol64 of the SNS names (abi/symbol_hash.h; social_frames_test recomputes them).
inline constexpr std::uint64_t kSymFriendStatusNotify = 0x26a19dc4d2d5579dULL;  // SNSFriendStatusNotify
inline constexpr std::uint64_t kSymFriendListResponse = 0xa78aeb2a4e89b10bULL;  // SNSFriendListResponse

inline constexpr std::size_t kFrameHeaderBytes = 24;

// What one call saw, for tests and counters.
struct FrameStats {
  std::size_t messages = 0;       // well-formed messages walked
  std::size_t consumed = 0;       // messages that changed a model
  std::size_t malformed = 0;      // 1 if the walk stopped on a bad marker or length
};

// Walks every message in `data` and applies the server-to-game ones to `ports`' models. Requests that
// the models ask for in answer (a name lookup, a friend-list refresh, a queued invite) go out through
// ports.send. Never throws and never reads past `len`.
FrameStats ObserveFrames(const Ports& ports, Direction direction, const std::uint8_t* data, std::size_t len,
                         std::uint64_t nowSeconds) noexcept;

}  // namespace quest_social
