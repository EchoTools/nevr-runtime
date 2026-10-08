// The facade slots that call into the game: Update (delivers the party callbacks), JoinInternal and
// AcceptInvite (ask the game's accept gate).
//
// Built with -fno-exceptions (and checked by tools/check_quest_social_frames.sh): the frames here carry
// no landing pad, no LSDA and no personality, so a game exception thrown by a callback unwinds through
// them by CFI alone. Everything that can throw (the model, the heap) is behind social_internal.h in
// social_facade.cpp, which contains its own failures and has returned before any call below.
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__cpp_exceptions)
#error "social_game_calls.cpp must be built with -fno-exceptions (see social_internal.h)"
#endif

#include "quest/social/social_abi.h"
#include "quest/social/social_internal.h"

namespace quest_social::internal {
namespace {

// The delegate proxies are MemberProxy<CR15NetGame, ...>(void* context, const void* buffer, args...).
// The array lives in the object at +8, 0x20 bytes per callback: context, 16 inline bytes, proxy function.
struct CallbackEntry {
  void* context;
  const void* buffer;
  void* function;
};

CallbackEntry EntryAt(void* self, std::size_t index) noexcept {
  CallbackEntry entry;
  const std::uint8_t* base = static_cast<const std::uint8_t*>(self) + 8 + kCallbackStride * index;
  std::memcpy(&entry.context, base, sizeof(void*));
  entry.buffer = base + 8;
  std::memcpy(&entry.function, base + 0x18, sizeof(void*));
  return entry;
}

template <typename Fn>
Fn FunctionOf(const CallbackEntry& entry) noexcept {
  Fn fn = nullptr;
  static_assert(sizeof(fn) == sizeof(entry.function), "function pointer size");
  std::memcpy(&fn, &entry.function, sizeof(fn));
  return fn;
}

void CallVoid(void* self, std::size_t index) noexcept {
  const CallbackEntry e = EntryAt(self, index);
  if (e.function == nullptr) return;
  NoteCallbackDelivered(self);
  FunctionOf<void (*)(void*, const void*)>(e)(e.context, e.buffer);
}

void CallU32(void* self, std::size_t index, std::uint32_t value) noexcept {
  const CallbackEntry e = EntryAt(self, index);
  if (e.function == nullptr) return;
  NoteCallbackDelivered(self);
  FunctionOf<void (*)(void*, const void*, std::uint32_t)>(e)(e.context, e.buffer, value);
}

void CallIdName(void* self, std::size_t index, std::uint64_t id, const char* name) noexcept {
  const CallbackEntry e = EntryAt(self, index);
  if (e.function == nullptr) return;
  NoteCallbackDelivered(self);
  FunctionOf<void (*)(void*, const void*, std::uint64_t, const char*)>(e)(e.context, e.buffer, id, name);
}

// PartyInvitationCB(LocalUserID, u32) -> u32 is the accept gate: nonzero lets the join go ahead. It reads
// neither argument (libr15 0x127053c), so both are zero. No callback registered: the join goes ahead.
bool CallGate(void* self) noexcept {
  const CallbackEntry e = EntryAt(self, kCbInviteAccepted);
  if (e.function == nullptr) return true;
  NoteCallbackDelivered(self);
  const std::uint32_t result =
      FunctionOf<std::uint32_t (*)(void*, const void*, std::uint32_t, std::uint32_t)>(e)(e.context, e.buffer, 0, 0);
  return result != 0;
}

void Dispatch(void* self, const PendingEvent& event) noexcept {
  switch (event.kind) {
    case kEvCreated: CallVoid(self, kCbCreated); break;
    case kEvJoined: CallVoid(self, kCbJoined); break;
    case kEvJoinFailed: CallU32(self, kCbJoinFailed, event.code); break;
    case kEvUpdated: CallVoid(self, kCbUpdated); break;
    case kEvHostChanged: CallVoid(self, kCbHostChanged); break;
    case kEvLeft: CallVoid(self, kCbLeft); break;
    case kEvKicked: CallVoid(self, kCbKicked); break;
    case kEvMemberJoined: CallU32(self, kCbMemberJoined, event.index); break;
    case kEvMemberUpdated: CallU32(self, kCbMemberUpdated, event.index); break;
    case kEvMemberLeft: CallIdName(self, kCbMemberLeft, event.id, event.name); break;
    case kEvInviteReceived: CallU32(self, kCbInviteReceived, 0); break;
  }
}

// JoinInternal in CNSOVRSocial's order: drop the invites to that party, defer if a create or join is in
// flight, run the accept gate, then join.
void JoinFlow(void* self, std::uint64_t partyId) noexcept {
  if (JoinBegin(self, partyId) == JoinStep::kDeferred) return;
  const bool allowed = CallGate(self);
  JoinFinish(self, partyId, allowed);
}

}  // namespace

void SlotUpdateEntry(void* self, const void* params) noexcept {
  if (self == nullptr) return;
  TraceSlotCall(self, kUpdate);
  const std::uint64_t deferred = UpdatePrepare(self, params);
  if (deferred != 0) JoinFlow(self, deferred);  // a deferred join takes the place of the create decision
  EventBatch batch;
  batch.count = 0;
  batch.dropped = 0;
  UpdateCollect(self, &batch);
  for (std::uint32_t i = 0; i < batch.count; ++i) Dispatch(self, batch.events[i]);
  UpdateFinish(self);
}

void SlotJoinInternalEntry(void* self, std::uint64_t partyId) noexcept {
  if (self == nullptr) return;
  TraceSlotCall(self, kJoinInternal);
  JoinFlow(self, partyId);
}

void SlotAcceptInviteEntry(void* self, std::uint32_t index) noexcept {
  if (self == nullptr) return;
  TraceSlotCall(self, kAcceptInvite);
  const std::uint64_t partyId = InvitePartyAt(self, index);
  if (partyId != 0) JoinFlow(self, partyId);
}

}  // namespace quest_social::internal
