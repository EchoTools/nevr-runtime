#pragma once
// The decisions VeilNet's PC send path takes (#2242, EchoCombat 2.0.6): which destination is the server (the game still sends to
// it), which datagrams go to it at all (only the hello, to a server that has not answered), and how many hellos a server that never
// answers gets. The sockets, the locks and the clock stay in veilnet.cpp; these are pure, so test_veilnet_gate.cpp drives them.
#include <cstddef>
#include <cstdint>

#include "runtime/patch/ec_rules.h"  // nevr_ec::KnownTacMod: the seven tac mods, one table (#2242)

namespace nevr_combat::gate {

// A destination is one server: its address and port in one key. The one packing, used by the code and by its tests.
inline constexpr int kServerKeyShift = 16;
inline constexpr std::uint64_t ServerKey(std::uint32_t addr, std::uint16_t port) {
  return std::uint64_t{addr} << kServerKeyShift | port;
}


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


// The local player's tac mod slot was set to `sym` (#2242, EchoCombat 2.0.8): a known tac mod that is not the selection already
// standing (nEVR re-fires the slot setter, which would otherwise restart the selection's re-run count each time).
inline bool PickFromSlot(std::uint64_t sym, std::uint64_t selected) {
  return nevr_ec::KnownTacMod(sym) && sym != selected;
}

// ---- SERIES-10 ONLY: the recursion guard and the standard-chassis flag (#2242, EchoCombat 2.0.5 and 2.0.8). Revault (echovr.exe):
// the flag reader is 0xCE4460, a wrapper over the symbol-keyed boolean lookup at 0xCE4390. Its one caller that matters is
// CR15NetCustomizationCS::LoadAndApplyItem (0xD0D170), which passes (cs+0xB8, the symbol) at 0xD0D1F4 and branches on the answer
// at 0xD0D1F9 (the return address). The symbol is the 8 bytes at 0x141CB3420, 0x29e261a1df774dd3.
inline constexpr std::uint64_t kStandardChassisFlag = 0x29e261a1df774dd3ull;
inline constexpr std::uintptr_t kNetFlagRva = 0xCE4460;
inline constexpr std::uintptr_t kLoadApplyFlagRetRva = 0xD0D1F9;   // the return address after the call to kNetFlagRva
inline constexpr std::uint8_t kNetFlagHead[14] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};

// The flag reads 0 for LoadAndApplyItem's own call, so the engine does not put the standard chassis on by itself (VeilNet puts
// SERIES-10 ONLY on instead). Any other caller, or any other symbol, gets the real answer.
inline bool FlagForcedZero(std::uint64_t sym, std::uintptr_t ret, std::uintptr_t callSiteRet) {
  return sym == kStandardChassisFlag && ret == callSiteRet;
}

// Put a chassis item on as SERIES-10 ONLY (his ChassisWanted, behind his SwapChassis): the podium rule is on, or the server's
// standard rule is set and the flag hook makes it SERIES-10. `depth` counts VeilNet's own chassis calls on this thread: a call made
// from inside one of them is never swapped again (the recursion guard: without it the engine's standard swap and ours fed each
// other until the stack ran out, EchoCombat 2.0.5).
inline bool SwapChassisWanted(bool chassisOn, bool standardSet, bool flagHooked, int depth) {
  if (depth != 0) return false;
  return chassisOn ? (flagHooked || !standardSet) : (flagHooked && standardSet);
}

}  // namespace nevr_combat::gate
