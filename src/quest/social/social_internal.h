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

#include "quest/social/social_abi.h"

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

// ---- party and member JSON ----------------------------------------------------------------------
// The game's CJson functions (social_abi.h GameJson) are called only from social_game_calls.cpp. The facade side
// builds a plan of POD operations, the game-call side runs it and records the outcome, and the facade side reads the
// outcome back. Texts are pointers into strings the facade keeps alive until the next plan (the member and party
// data it has loaded), so nothing with a destructor is live across a game call.

inline constexpr std::uint8_t kJsonParty = 0xFF;  // JsonOp::slot of the party CJson (+0x1f0); 0..9 are member slots
inline constexpr std::size_t kJsonOpSlots = kMemberJsonSlots + 1;

enum JsonOpKind : std::uint8_t { kJsonNone, kJsonLoad, kJsonClear };

struct JsonOp {
  JsonOpKind kind;
  std::uint8_t slot;
  std::uint8_t ok;       // written by the game-call side: the game function returned 0
  const char* text;      // kJsonLoad: the JSON text (not NUL-terminated)
  std::uint64_t length;
};

struct JsonPlan {
  GameJson game;
  std::uint32_t count;
  JsonOp ops[kJsonOpSlots];
};

inline constexpr std::size_t kShareBufferBytes = 16 * 1024;

// The party and local-member data the game wrote, to be read out with EncodeToCompact and sent. The buffers belong to
// the facade.
struct ShareJob {
  GameJson game;
  std::uint8_t party;      // encode the party CJson
  std::uint8_t member;     // encode the local member's CJson (member slot 0)
  std::uint8_t partyOk;    // written by the game-call side: EncodeToCompact returned 0
  std::uint8_t memberOk;
  char* partyBuffer;
  char* memberBuffer;
  std::uint64_t capacity;  // kShareBufferBytes - 1: the NUL fits after the longest accepted text
  std::uint64_t partySize;
  std::uint64_t memberSize;
};

enum class JoinStep : std::uint8_t { kDeferred, kAskGate };

// One script event post for a UI slot. `symbol` 0: nothing to post. `result` is written by the game-call side.
enum class UiEventResult : std::uint8_t { kNotRun, kPosted, kNoFunction, kNoNetGame };
struct UiEventJob {
  GameEvents game;
  std::uint64_t symbol;
  std::uint32_t slot;
  UiEventResult result;
};

// ---- implemented in social_facade.cpp: every one is noexcept and contains its own failures ---------
//
// `self` is the object the game holds. A null or foreign object is answered with the neutral value.

void TraceSlotCall(void* self, std::size_t slot) noexcept;
// A callback of the game was just called: `callback` is its Callback index (social_abi.h).
void NoteCallbackDelivered(void* self, std::size_t callback) noexcept;

// Update, first half: the join the model deferred (returned, 0 if none), else the party-create decision.
std::uint64_t UpdatePrepare(void* self, const void* params) noexcept;
// Update, second half: publish the model, mirror the object fields, drain the events into `out`, and plan the
// party and member data the game's JSON must load (`json`).
void UpdateCollect(void* self, EventBatch* out, JsonPlan* json) noexcept;
// The plan has run: count and log what failed.
void JsonApplied(void* self, const JsonPlan* json) noexcept;
// After the callbacks: which of the game's written data has to be shared (party data the leader wrote, the local
// member's data), then (after the game-call side encoded it) validate and send it.
void ShareBegin(void* self, ShareJob* job) noexcept;
void ShareFinish(void* self, const ShareJob* job) noexcept;
// Update, last step: the host's joinable bit against the server's lock.
void UpdateFinish(void* self) noexcept;

// Reset: leave the party silently and put the object's own fields back (the game follows with AddMember).
// Returns the game's CJson::Reset to call on the party CJson at +0x1f0, or nullptr when it is not known (it
// is counted); the call itself is made by SlotResetEntry, outside any frame with a landing pad.
CJsonResetFn ResetPrepare(void* self) noexcept;

// Join: begin (drop the invites to that party, defer if a create or join is in flight) ...
JoinStep JoinBegin(void* self, std::uint64_t partyId) noexcept;
// ... and finish after the game's accept gate answered.
void JoinFinish(void* self, std::uint64_t partyId, bool allowed) noexcept;
// The party of the invite the game lists at `index`, newest first (0 none).
std::uint64_t InvitePartyAt(void* self, std::uint32_t index) noexcept;
// A UI slot that asks the game's tablet for its Friends tab: the facade's own behaviour for the slot (its log line),
// then the event to post (job->symbol, job->game). ... and after the game-call side ran it, the outcome is logged.
void UiEventBegin(void* self, std::uint32_t slot, std::uint64_t target, UiEventJob* job) noexcept;
void UiEventFinish(void* self, const UiEventJob* job) noexcept;

// ---- implemented in social_game_calls.cpp (built -fno-exceptions): the slots that call the game ----
void SlotUpdateEntry(void* self, const void* params) noexcept;
void SlotJoinInternalEntry(void* self, std::uint64_t partyId) noexcept;
void SlotAcceptInviteEntry(void* self, std::uint32_t index) noexcept;
void SlotResetEntry(void* self) noexcept;
// The party tab's "Invite Members" slots (#318 probe): OpenNewSendInviteUI(user) (slot 40) and OpenPartyUI(user,
// target) (slot 44). They keep their facade behaviour (no invite from the call) and post the script event that is the
// candidate for switching the tablet to its Friends tab, through the game function in GameEvents.
void SlotInviteUiNoTargetEntry(void* self, std::uint32_t user) noexcept;
void SlotPartyUiTargetEntry(void* self, std::uint32_t user, std::uint64_t target) noexcept;

}  // namespace quest_social::internal
