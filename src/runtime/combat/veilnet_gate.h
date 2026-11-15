#pragma once
// The decisions VeilNet's PC send path takes (#2242, EchoCombat 2.0.6): which destination is the server (the game still sends to
// it), which datagrams go to it at all (only the hello, to a server that has not answered), and how many hellos a server that never
// answers gets. The sockets, the locks and the clock stay in veilnet.cpp; these are pure, so test_veilnet_gate.cpp drives them.
#include <cstdint>

namespace nevr_combat::gate {

// A destination the game sent to within this long (ms) is the server; one it has left gets nothing from VeilNet. Before 2026-11-08
// the last server was kept for good and got a hello every 2 s for as long as the game ran: one idle player's game hit a restarting
// stock server for 10 hours.
inline constexpr std::uint64_t kDestQuietMs = 3000;
// Hellos to a server that has not answered one: the count stops at this many, until the game connects to another server or it answers.
inline constexpr int kHelloTries = 5;

// The game last sent to a destination at `at` (ms; 0: never). True within `window` of `now`.
inline bool Recent(std::uint64_t at, std::uint64_t now, std::uint64_t window) { return at != 0 && now - at < window; }

// Only the hello goes to a server that has not answered one: everything else (the veil, RULES asks, map switch, ...) waits for the
// server to state that it runs EchoCombat. `answered` is that statement (nevr_server_features::Client().Current().echoCombat).
inline bool SendAllowed(std::uint8_t type, bool answered) { return type == 'H' || answered; }

enum class Hello { kNone, kSend, kSendAndGiveUp };

// The hello count for the server the game is talking to. Call once per hello tick with that server's key (0: none) and whether it has
// answered. kSendAndGiveUp is the last hello of an unanswered server (VeilNet logs that it has no EchoCombat there).
class HelloTries {
 public:
  Hello Next(std::uint64_t server, bool answered) {
    if (server == 0) {
      server_ = 0;
      unanswered_ = 0;
      return Hello::kNone;
    }
    if (server != server_) {
      server_ = server;
      unanswered_ = 0;
    } else if (answered) {
      unanswered_ = 0;
    }
    if (unanswered_ >= kHelloTries) return Hello::kNone;
    ++unanswered_;
    if (unanswered_ == kHelloTries && !answered) return Hello::kSendAndGiveUp;
    return Hello::kSend;
  }

 private:
  std::uint64_t server_ = 0;
  int unanswered_ = 0;
};


// The seven tac mods' symbols (EchoCombat's names; the same values as ec_rules.h's AbilitySymbol). A selection is one of these.
inline constexpr std::uint64_t kTacModSymbols[] = {
    0xe32dc7c9d4804fb5ull, 0xe32dc7cad6864aa4ull, 0xc8c33e48386618b6ull, 0xc8c33e4832761fbcull,
    0x41d2d43513260b1aull, 0x41d2d4381430080cull, 0x41d2d02f1b2a1316ull,
};
inline bool KnownTacMod(std::uint64_t sym) {
  for (std::uint64_t k : kTacModSymbols)
    if (k == sym) return true;
  return false;
}

// The local player's tac mod slot was set to `sym` (#2242, EchoCombat 2.0.8): a known tac mod that is not the selection already
// standing (nEVR re-fires the slot setter, which would otherwise restart the selection's re-run count each time).
inline bool PickFromSlot(std::uint64_t sym, std::uint64_t selected) { return KnownTacMod(sym) && sym != selected; }

}  // namespace nevr_combat::gate
