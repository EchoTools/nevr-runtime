#pragma once
// The early quit lockout's decisions, apart from the hooks that apply them (early_quit_lockout.cpp), so they
// are unit-tested without the game. Field map and evidence:
// echovr-reconstruction docs/earlyquit_field_analysis.md (dc719052).
//
// SNSEarlyQuitUpdateNotification (0x38 bytes) carries two 64-bit fields the game treats differently:
//   +0x18  a stamp: CR15NetGame::SetEarlyQuitPenaltyLevel (echovr.exe 0x1401ae2e0) stores it at netGame+0x64820
//          and applies an update only when nothing is stored (-1) or the stored stamp is smaller (signed). An
//          update whose stamp is not newer is dropped whole, counters and levels included. The game service
//          sends a stamp above the one before, for each player; the login profile's `earlyquit|penaltyts` is
//          a stamp of the same kind.
//   +0x20  the lockout's absolute expiry in Unix seconds, -1 or 0 for none. EarlyQuitUpdateCB (0x1401618e0)
//          passes it to the setter as its third argument (R8); the login path
//          (CR14LocalPlayerCS_ParseEarlyQuitData, 0x14017e330) passes -1. The setter never reads it and writes
//          -1 to the countdown expiry the scripts read (netGame+0x64828), so the hook writes the expiry there.
// The layout the runtime reads is the stamp at +0x18 and the expiry at +0x20, with the expiry 0 or -1 for none.
// The game service's layout in nakama main is a second one: it writes the lockout's end in the stamp field (+0x18),
// zero at +0x20, and a clear as a stamp of 0. The setter's gate would drop such a clear, and a shorter legacy
// lockout than the one stored, so the hook opens the gate for a legacy-shaped update by writing -1 to the stored
// stamp (netGame+0x64820) before it calls the setter. ReadIntake tells the two layouts apart.
// The fixture shared with the game service's test is in test_early_quit_lockout.cpp.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nevr_early_quit_lockout {

constexpr std::size_t kUpdateSize = 0x38;
constexpr std::size_t kStampOffset = 0x18;
constexpr std::size_t kExpiryOffset = 0x20;
constexpr std::size_t kNumEarlyQuitsOffset = 0x28;
constexpr std::size_t kNumSteadyMatchesOffset = 0x2C;
constexpr std::size_t kNumSteadyEarlyQuitsOffset = 0x30;
constexpr std::size_t kPenaltyLevelOffset = 0x34;
constexpr std::size_t kSteadyPlayerLevelOffset = 0x35;

// The fields of an update the client reads (the first 0x18 bytes are the session and player ids, not read).
struct Update {
  std::int64_t stamp;
  std::int64_t expiry;
  std::int32_t numEarlyQuits;
  std::int32_t numSteadyMatches;
  std::int32_t numSteadyEarlyQuits;
  std::uint8_t penaltyLevel;
  std::uint8_t steadyPlayerLevel;
};

// Reads an update from its wire bytes (little-endian, as on every host the runtime runs on). False, and `out`
// untouched, when the buffer is missing or shorter than the message.
inline bool ParseUpdate(const std::uint8_t* bytes, std::size_t size, Update* out) {
  if (bytes == nullptr || out == nullptr || size < kUpdateSize) return false;
  Update update{};
  std::memcpy(&update.stamp, bytes + kStampOffset, sizeof(update.stamp));
  std::memcpy(&update.expiry, bytes + kExpiryOffset, sizeof(update.expiry));
  std::memcpy(&update.numEarlyQuits, bytes + kNumEarlyQuitsOffset, sizeof(update.numEarlyQuits));
  std::memcpy(&update.numSteadyMatches, bytes + kNumSteadyMatchesOffset, sizeof(update.numSteadyMatches));
  std::memcpy(&update.numSteadyEarlyQuits, bytes + kNumSteadyEarlyQuitsOffset, sizeof(update.numSteadyEarlyQuits));
  update.penaltyLevel = bytes[kPenaltyLevelOffset];
  update.steadyPlayerLevel = bytes[kSteadyPlayerLevelOffset];
  *out = update;
  return true;
}

// The setter's gate (0x1401ae2f0-0x1401ae2f9: CMP RAX,-1; JE take; CMP RAX,RDX; JGE skip).
inline bool SetterTakes(std::int64_t storedBefore, std::int64_t stamp) { return storedBefore == -1 || storedBefore < stamp; }

// Compatibility with the game service that writes the lockout's end in the stamp field. Remove it, with
// ReadIntake's use in SetPenaltyHook and the legacy tests, once nakama main writes the stamp-and-expiry layout
// (nakama branch feat/712-early-quit-stamp-expiry); tracked in #1830.
// A legacy lockout is read only when its end is further off than the clocks of client and service can differ.
constexpr std::int64_t kLegacySlackSec = 30;

struct Intake {
  bool legacy;          // the update is in nakama main's layout: the end is in the stamp field and +0x20 is zero
  std::int64_t expiry;  // the lockout's end in Unix seconds; 0 or less for none
};

inline Intake ReadIntake(std::int64_t stamp, std::int64_t expiry, std::int64_t now) {
  if (expiry == 0 && stamp > now + kLegacySlackSec) return {true, stamp};  // legacy lockout: stamp is the end
  if (expiry == 0 && stamp <= 0) return {true, 0};                         // legacy clear: stamp 0, nothing in force
  return {false, expiry};                                                  // the stamp-and-expiry layout, or the login path (-1)
}

// What the countdown should be once the setter has run.
struct Countdown {
  bool applied;         // the setter stored this update; when false the hook leaves the countdown as it is
  std::int64_t expiry;  // netGame+0x64828: -1 for none
  bool active;          // flags bit 45, which CR15NetGame::Update (0x1401bbdb0) clears at expiry
};

// storedBefore and storedAfter: netGame+0x64820 before and after the setter ran; stamp and expiry: what the
// update carried (the login path's expiry is -1); now: Unix seconds. The update was applied when the setter's
// gate was open and the stamp is what is stored afterwards; the hook reads storedBefore ahead of the call
// because a repeated stamp leaves storedAfter equal to it too.
inline Countdown AfterSetPenalty(std::int64_t storedBefore, std::int64_t storedAfter, std::int64_t stamp, std::int64_t expiry,
                                 std::int64_t now) {
  if (!SetterTakes(storedBefore, stamp) || storedAfter != stamp) return {false, -1, false};
  if (expiry > now) return {true, expiry, true};
  return {true, -1, false};  // none (-1 or 0) or already over
}

// The penalty expression's lockoutcountdownactive output, which the game hard-codes to 0 (0x140d96319): on
// while the countdown has seconds left.
inline bool CountdownActive(std::int64_t countdownSec) { return countdownSec > 0; }

}  // namespace nevr_early_quit_lockout
