#pragma once
// The error page for a lobby session failure whose code the game shows as a fixed literal (#1443). The game's own
// failure callback (CR15NetClientLobby::LobbySessionFailureCB, echovr.exe 0x14017d380) switches on the failure code:
// codes 11 and 12 pass the service's message to the error block, every other code only changes the lobby state and
// drops the message. This header holds the pure rules: which codes take the service's message, and how the error block
// (CR15NetGame::SetErrorMessage, echovr.exe 0x1401ae5e0) is laid out. The hook that applies them is in
// lobby_full_backoff.cpp.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nevr_lobby_failure_message {

// The failure payload's message: a 64-byte field at payload+0x20 (lobby_frame.h, kFailureMessageAt).
constexpr std::size_t kMessageOffset = 0x20;
constexpr std::size_t kMessageSize = 64;

// The error block SetErrorMessage writes on CR15NetGame (echovr.exe 0x1401ae5e0 and 0x1401aeb50): the flag byte at
// +0x63308, then four 64-byte lines from +0x63309. Each line ends in NUL at its 64th byte (the same layout as the
// Quest build's game_layout in src/quest/sentinel/pinned_targets.h).
constexpr std::size_t kErrorBlockOffset = 0x63308;
constexpr std::size_t kErrorLineBytes = 0x40;
constexpr std::size_t kErrorLines = 4;
constexpr std::uint8_t kErrorFlagFourLines = 1;

// Codes 0 to 10 and 13 show a fixed literal in the game (the lobby-state page). Codes 11 and 12 already show the
// service's message, so they are not listed here.
constexpr std::uint8_t kFirstLiteralCode = 0;
constexpr std::uint8_t kLastLiteralCode = 10;
constexpr std::uint8_t kAccessDeniedCode = 13;

inline bool TakesServiceMessage(std::uint8_t code) {
  return (code >= kFirstLiteralCode && code <= kLastLiteralCode) || code == kAccessDeniedCode;
}

// The game's name for a failure code, the same words its own "[NETLOBBY] Lobby session failure: %s" line uses
// (LobbySessionFailureCB's switch). Used as line 1 of the error block.
inline const char* Title(std::uint8_t code) {
  switch (code) {
    case 0: return "unspecified";
    case 1: return "unsupported version";
    case 2: return "bad request";
    case 3: return "timeout";
    case 4: return "lobby invalid";
    case 5: return "lobby incompatible";
    case 6: return "lobby no crossplay";
    case 7: return "lobby locked";
    case 8: return "lobby full";
    case 9: return "internal error";
    case 10: return "missing entitlement";
    case 13: return "access denied";
    default: return "unknown";
  }
}

// The message's text before its NUL, at most the field. False when the field is empty, so the game's own page stays.
inline bool HasMessage(const char* field, std::size_t fieldSize) {
  return field != nullptr && fieldSize > 0 && field[0] != '\0';
}

// Bytes of the message before its NUL, at most the field. Logged in place of the text (logging.md Rule 4).
inline std::size_t MessageBytes(const char* field, std::size_t fieldSize) {
  std::size_t n = 0;
  while (field != nullptr && n < fieldSize && field[n] != '\0') ++n;
  return n;
}

// Writes the error block at gameBase+kErrorBlockOffset: line 1 is the title, line 2 the service's message, lines 3
// and 4 are empty. Each line is cut to 63 bytes and NUL-terminated. Returns false, and writes nothing, when the
// message is empty or the base is null.
inline bool WriteErrorBlock(std::uint8_t* gameBase, const char* title, const char* field, std::size_t fieldSize) {
  if (gameBase == nullptr || !HasMessage(field, fieldSize)) return false;
  std::uint8_t* block = gameBase + kErrorBlockOffset;
  block[0] = kErrorFlagFourLines;
  std::uint8_t* lines = block + 1;
  std::memset(lines, 0, kErrorLines * kErrorLineBytes);
  const std::size_t titleLen = std::strlen(title);
  std::memcpy(lines, title, titleLen < kErrorLineBytes - 1 ? titleLen : kErrorLineBytes - 1);
  const std::size_t messageLen = MessageBytes(field, fieldSize);
  std::memcpy(lines + kErrorLineBytes, field, messageLen < kErrorLineBytes - 1 ? messageLen : kErrorLineBytes - 1);
  return true;
}

}  // namespace nevr_lobby_failure_message
