// The party-invite gate on Quest: the same override the PC applies (src/runtime/patch/party_invite_gate.h).
//
// The game refuses a party invite before it reaches the social object unless the local player's profile JSON says
// `npe|firstmatch|completed` is true: CR15NetGame::PartySendInvite (libr15 0x129c7d8..0x129c800), OpenInviteUI
// (0x129c9c8..), OpenNewInviteUI (0x129ca6c.., 0x129cb20..), OpenPartyUI (0x129cd1c.., 0x129cdd0..), PartyLobbyUnjoinable
// (0x1259488..), DeepLinkCB (0x126efb4..) and CreateMatch (0x126f1b8..) each read it with
// `CJson::Boolean(profile, "npe|firstmatch|completed", 0, 0)` and, when it is false, raise the not-completed error
// instead of calling the social object. CR15NetGame::LogInSuccess (0x126cc38..0x126cc94) writes the flag true only
// for an account whose two profile stats do not sum to zero (it has played a match) or when a configuration bit is
// set ([netgame+0x2da0] byte 1 bit 0); a community account starts with no stats, so the gate would stay shut for
// exactly the players who want to invite a friend. The PC forces the read true; this does the same.
//
// The seam is libr15's own JUMP_SLOT for `CJson::Boolean(char const*, unsigned, unsigned) const` (0x36f6988, BIND_NOW,
// one relocation for the symbol; every call above goes through libr15's PLT stub at 0xf3e540). The handler calls the
// original and returns true for that one path; every other path, and every other CJson, is untouched. It never logs: it
// counts (calls, forced), and the reporter thread logs the counters.
//
// Hook frequency: CJson::Boolean is a general profile and settings reader, so this runs wherever the game reads a
// boolean, possibly every frame. The handler is a comparison of the path against a 24-byte literal (the first byte
// decides almost every call) and one relaxed atomic increment in the thunk.
#pragma once

#include <atomic>
#include <cstdint>

#include "callback_thunk.h"
#include "got_hook.h"

namespace quest_social {

inline constexpr const char* kFirstMatchPath = "npe|firstmatch|completed";
inline constexpr const char* kBooleanSymbol = "_ZNK10NRadEngine5CJson7BooleanEPKcjj";
inline constexpr std::uint64_t kBooleanSlotVaddr = 0x36f6988ULL;

// libr15's slot for CJson::Boolean, pinned to the artifact's build id and link-time address.
sentinel::GotTarget LibR15Boolean();

// The value the game's read should return: true for the gate's path, `original` for everything else. Pure.
std::uint32_t GateResult(const char* path, std::uint32_t original) noexcept;

struct GateTag {};
using GateSig = std::uint32_t(const void* json, const char* path, std::uint32_t defaultValue, std::uint32_t logMissing);
using GateThunk = sentinel::CallbackThunk<GateTag, GateSig>;

// The hook's handler, declared so the record (NEVR_HOOK_RECORD in social_invite_gate.cpp) and a test's own record can
// name it.
std::uint32_t OnBooleanHandler(GateThunk::Fn original, const void* json, const char* path, std::uint32_t defaultValue,
                               std::uint32_t logMissing) noexcept;

struct GateCounters {
  const std::atomic<std::uint64_t>& forced;  // reads of the gate's path that were false in the profile and returned true
};
GateCounters InviteGateCounters() noexcept;
void ResetInviteGateCountersForTest() noexcept;

// Registers the thunk's call counter (the hook is live) and `forced` with the reporter (hook_report.h): 2 counters.
// Call before StartReporter; returns false if either registration was refused.
bool RegisterInviteGateCounters();

// Arms the handler and redirects the slot. Logs the outcome. Returns the GotHook status (kOk: installed).
sentinel::GotStatus InstallInviteGate();

}  // namespace quest_social
