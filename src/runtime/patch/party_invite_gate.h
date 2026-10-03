#pragma once

#include <cstdint>
#include <cstring>

// The game refuses a party invite before it reaches the social object (FUN_14018aa90, the friend
// row's "+" handler) unless the local player's JSON profile says npe|firstmatch|completed is true;
// it dispatches delegate_onpartyinviteerrorfirstmatchnotcompleted (0x267b06d41014c304) instead and
// the facade's SendInvite slot is never called. The flag is normally written by the provider's
// login once the account has played a match. Accounts on community services start with no
// stats, so the gate would stay shut for exactly the players who want to invite a friend.

namespace PartyInviteGate {

constexpr const char* kFirstMatchPath = "npe|firstmatch|completed";
constexpr std::uint64_t kErrorFirstMatchNotCompleted = 0x267b06d41014c304ULL;
constexpr std::uint64_t kErrorOffline = 0xf8856ed68e71cdbfULL;

inline bool IsFirstMatchPath(const char* path) {
  return path != nullptr && std::strcmp(path, kFirstMatchPath) == 0;
}

/// Value the game's CJson::Boolean should return: the gate reads true, everything else is untouched.
inline std::uint32_t BooleanResult(const char* path, std::uint32_t original) {
  return IsFirstMatchPath(path) ? 1u : original;
}

/// Installs the CJson::Boolean override and the event-dispatch trace.
void Install(std::uintptr_t gameBase);

/// True once the friend invite handler (0x14018aa90) was prologue-validated and detoured for tracing.
/// Its first bytes are then our jump, so a caller that validates the prologue itself must accept this.
bool InviteHandlerTraced();

}  // namespace PartyInviteGate
