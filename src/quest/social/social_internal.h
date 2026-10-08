// The seam between the facade's two translation units.
//
// A game exception must never unwind through a frame that has a landing pad or an LSDA: the
// sentinel statically links LLVM libunwind and its personality, the game uses libgcc's through
// libc++_shared, and either would read the other's unwinder context as its own (callback_thunk.h,
// "Exceptions"). The facade calls into the game in exactly three places: the callbacks `Update`
// delivers, the accept gate `JoinInternal`/`AcceptInvite` ask, and nothing else. Those calls live in
// social_game_calls.cpp, which is built with -fno-exceptions and calls the functions below, which live
// in social_facade.cpp (built with exceptions, so they contain their own failures) and return before
// the game is called. No frame with a landing pad is live across a call into the game.
//
// This header names no standard-library type, so the no-exceptions translation unit instantiates
// nothing that could throw.
#pragma once

#include <cstddef>
#include <cstdint>

namespace quest_social::internal {

enum EventKind : std::uint8_t {
  kEvCreated,
  kEvJoined,
  kEvJoinFailed,       // code
  kEvUpdated,
  kEvHostChanged,
  kEvLeft,
  kEvKicked,
  kEvMemberJoined,     // index
  kEvMemberUpdated,    // index
  kEvMemberLeft,       // id, name
  kEvInviteReceived,
};

inline constexpr std::size_t kMaxPendingEvents = 32;
inline constexpr std::size_t kEventNameBytes = 64;

struct PendingEvent {
  EventKind kind;
  std::uint32_t index;
  std::uint32_t code;
  std::uint64_t id;
  char name[kEventNameBytes];
};

// The events one Update delivers, copied out of the model so no heap object is alive while the game
// runs. More than kMaxPendingEvents in one frame drops the rest and counts them.
struct EventBatch {
  std::uint32_t count;
  std::uint32_t dropped;
  PendingEvent events[kMaxPendingEvents];
};

enum class JoinStep : std::uint8_t { kDeferred, kAskGate };

// ---- implemented in social_facade.cpp: every one is noexcept and contains its own failures ---------
//
// `self` is the object the game holds. A null or foreign object is answered with the neutral value.

void TraceSlotCall(void* self, std::size_t slot) noexcept;
void NoteCallbackDelivered(void* self) noexcept;

// Update, first half: the join the model deferred (returned, 0 if none), else the party-create decision.
std::uint64_t UpdatePrepare(void* self, const void* params) noexcept;
// Update, second half: publish the model, mirror the object fields, drain the events into `out`.
void UpdateCollect(void* self, EventBatch* out) noexcept;
// Update, last step: the host's joinable bit against the server's lock.
void UpdateFinish(void* self) noexcept;

// Join: begin (drop the invites to that party, defer if a create or join is in flight) ...
JoinStep JoinBegin(void* self, std::uint64_t partyId) noexcept;
// ... and finish after the game's accept gate answered.
void JoinFinish(void* self, std::uint64_t partyId, bool allowed) noexcept;
// The party of the invite the game lists at `index`, newest first (0 none).
std::uint64_t InvitePartyAt(void* self, std::uint32_t index) noexcept;

// ---- implemented in social_game_calls.cpp (built -fno-exceptions): the slots that call the game ----
void SlotUpdateEntry(void* self, const void* params) noexcept;
void SlotJoinInternalEntry(void* self, std::uint64_t partyId) noexcept;
void SlotAcceptInviteEntry(void* self, std::uint32_t index) noexcept;

}  // namespace quest_social::internal
