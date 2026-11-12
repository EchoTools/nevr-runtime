// peer_message.h: nEVR's control messages between a dedicated server and its clients on the game's own broadcaster
// (#2365). The rules (codec, refusals, who is taken) are runtime/compat/peer_message_rules.h; the game's addresses are
// runtime/compat/peer_message_targets.h. This is the PC half: it registers the listener, sends, and logs.
//
// What runs, in this slice (nothing existing changes):
//   - Both roles listen for NEVRProtobufMessageV1 on the net game's peer broadcaster (NetGame+0x28c8) once it exists.
//   - The server sends `capabilities` (the gear its features offer) and a `ping` to each peer its match accepts, once per
//     peer per session.
//   - A client that takes `capabilities` from the authority peer pings it back; either side answers a `ping` with a
//     `pong` to its sender.
// Every line is `[NEVR.PEER]`: one Info line per message sent or taken, a Warning (bounded, counted) per refusal.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nevr_peer_message {

/// What the server knows of its match, from its lobby (runtime/server/server_context.h). Read on the game thread.
struct MatchView {
  bool valid = false;                    // a lobby exists
  std::string session;                   // the session id ("" between sessions)
  std::vector<std::uint64_t> peers;      // the broadcaster peer of each accepted entrant (join state 4)
  std::uintptr_t lobbyBroadcaster = 0;   // the lobby's CBroadcaster*, logged against the net game's once
};

/// The server's source of its match. Set once by the game server; a client has none.
void SetMatchViewSource(std::function<MatchView()> source);

/// Called from the per-frame dispatcher (runtime/frame/tick.cpp) on the game thread. Cheap when there is nothing to do:
/// two pointer reads, and the server's match read at most every 250 ms.
void Poll(std::uint64_t nowUs);

}  // namespace nevr_peer_message
