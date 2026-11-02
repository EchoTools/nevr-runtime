// The facade slots that call into the game: Update (delivers the party callbacks), JoinInternal and
// AcceptInvite (ask the game's accept gate), Reset (the game's CJson::Reset on the party CJson).
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
  NoteCallbackDelivered(self, index);
  FunctionOf<void (*)(void*, const void*)>(e)(e.context, e.buffer);
}

void CallU32(void* self, std::size_t index, std::uint32_t value) noexcept {
  const CallbackEntry e = EntryAt(self, index);
  if (e.function == nullptr) return;
  NoteCallbackDelivered(self, index);
  FunctionOf<void (*)(void*, const void*, std::uint32_t)>(e)(e.context, e.buffer, value);
}

void CallIdName(void* self, std::size_t index, std::uint64_t id, const char* name) noexcept {
  const CallbackEntry e = EntryAt(self, index);
  if (e.function == nullptr) return;
  NoteCallbackDelivered(self, index);
  FunctionOf<void (*)(void*, const void*, std::uint64_t, const char*)>(e)(e.context, e.buffer, id, name);
}

// PartyInvitationCB(LocalUserID, u32) -> u32 is the accept gate: nonzero lets the join go ahead. It reads
// neither argument (libr15 0x127053c), so both are zero. No callback registered: the join goes ahead.
bool CallGate(void* self) noexcept {
  const CallbackEntry e = EntryAt(self, kCbInviteAccepted);
  if (e.function == nullptr) return true;
  NoteCallbackDelivered(self, kCbInviteAccepted);
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

std::uint8_t* MemberJsonBase(void* self) noexcept {
  std::uintptr_t base = 0;
  std::memcpy(&base, static_cast<const std::uint8_t*>(self) + kOffMemberJson, sizeof(base));
  return reinterpret_cast<std::uint8_t*>(base);
}

void* JsonSlot(void* self, std::uint8_t slot) noexcept {
  if (slot == kJsonParty) return static_cast<std::uint8_t*>(self) + kOffPartyJson;
  return MemberJsonBase(self) + 16 * static_cast<std::size_t>(slot);
}

// Runs the data plan: each load is the game's CJson::DecodeFrom on the member's or the party's CJson (it replaces
// what that CJson held), each clear its CJson::Reset. A function the game does not offer (nullptr) leaves the
// operation unrun and not ok.
void RunPlan(void* self, JsonPlan* plan) noexcept {
  for (std::uint32_t i = 0; i < plan->count; ++i) {
    JsonOp& op = plan->ops[i];
    op.ok = 0;
    if (op.slot != kJsonParty && op.slot >= kMemberJsonSlots) continue;
    void* json = JsonSlot(self, op.slot);
    if (op.kind == kJsonLoad && plan->game.decode != nullptr) {
      op.ok = plan->game.decode(json, op.text, op.length) == 0 ? 1 : 0;
    } else if (op.kind == kJsonClear && plan->game.reset != nullptr) {
      plan->game.reset(json);
      op.ok = 1;
    }
  }
}

// What the plan loaded reaches the game as it did natively: a member whose data changed is MemberUpdated, the party's
// data is Updated.
void DeliverLoaded(void* self, const JsonPlan& plan) noexcept {
  for (std::uint32_t i = 0; i < plan.count; ++i) {
    const JsonOp& op = plan.ops[i];
    if (op.kind != kJsonLoad || op.ok == 0) continue;
    if (op.slot == kJsonParty) {
      CallVoid(self, kCbUpdated);
    } else {
      CallU32(self, kCbMemberUpdated, op.slot);
    }
  }
}

// Reads the written data out of the game's CJson into the facade's buffers.
void RunShare(void* self, ShareJob* job) noexcept {
  job->partyOk = 0;
  job->memberOk = 0;
  if (job->game.encode == nullptr) return;
  if (job->party != 0) {
    unsigned long long size = job->capacity;
    job->partyOk = job->game.encode(JsonSlot(self, kJsonParty), job->partyBuffer, &size, 0, "") == 0 && size < job->capacity ? 1 : 0;
    job->partySize = size;
  }
  if (job->member != 0) {
    unsigned long long size = job->capacity;
    job->memberOk = job->game.encode(JsonSlot(self, 0), job->memberBuffer, &size, 0, "") == 0 && size < job->capacity ? 1 : 0;
    job->memberSize = size;
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
  JsonPlan json;
  json.count = 0;
  UpdateCollect(self, &batch, &json);
  // Received data is loaded before the events fire, so a MemberJoined callback already finds the member's data
  // (PartyMemberJoinedCB reads its headsettype), as the PC does.
  RunPlan(self, &json);
  JsonApplied(self, &json);
  for (std::uint32_t i = 0; i < batch.count; ++i) Dispatch(self, batch.events[i]);
  DeliverLoaded(self, json);
  // What the callbacks (and the game's own Update) wrote into the party and member CJson goes to the server.
  ShareJob share;
  share.party = 0;
  share.member = 0;
  ShareBegin(self, &share);
  RunShare(self, &share);
  ShareFinish(self, &share);
  UpdateFinish(self);
}

void SlotJoinInternalEntry(void* self, std::uint64_t partyId) noexcept {
  if (self == nullptr) return;
  TraceSlotCall(self, kJoinInternal);
  JoinFlow(self, partyId);
}

// Reset: the facade side leaves the party and clears the object's own fields, then the game's own
// CJson::Reset runs on the party CJson at +0x1f0, as CNSISocial::Reset does (libpnsovr 0x36a92c). That
// CJson belongs to the social object (its constructor builds it, 0x203254, its destructor destroys it,
// 0x20845c), and the game's Update fills it while this client leads a party, so the tree it holds must be
// freed by the game's own code, here, in a frame with no landing pad.
void SlotResetEntry(void* self) noexcept {
  if (self == nullptr) return;
  TraceSlotCall(self, kReset);
  const CJsonResetFn reset = ResetPrepare(self);
  if (reset == nullptr) return;
  reset(static_cast<std::uint8_t*>(self) + kOffPartyJson);
  // The member CJson array too (CNSISocial::Reset resets one per member, libpnsovr 0x36a96c..0x36a988): the
  // game wrote the local member's into the first, and loaded data sits in the others. A zeroed one is a no-op.
  for (std::size_t i = 0; i < kMemberJsonSlots; ++i) reset(MemberJsonBase(self) + 16 * i);
}

namespace {

// The delegate context is the CR15NetGame (see CallbackEntry): the first registered delegate's context.
void RunUiEvent(void* self, UiEventJob* job) noexcept {
  job->result = UiEventResult::kNoFunction;
  if (job->symbol == 0 || job->game.send == nullptr) return;
  void* const netGame = EntryAt(self, kCbCreated).context;
  job->result = UiEventResult::kNoNetGame;
  if (netGame == nullptr) return;
  job->game.send(netGame, job->symbol);
  job->result = UiEventResult::kPosted;
}

}  // namespace

void SlotInviteUiNoTargetEntry(void* self, std::uint32_t user) noexcept {
  (void)user;
  if (self == nullptr) return;
  TraceSlotCall(self, kOpenNewSendInviteUI);
  UiEventJob job;
  job.game = GameEvents{};
  job.symbol = 0;
  job.slot = kOpenNewSendInviteUI;
  job.result = UiEventResult::kNotRun;
  UiEventBegin(self, kOpenNewSendInviteUI, 0, &job);
  RunUiEvent(self, &job);
  UiEventFinish(self, &job);
}

void SlotPartyUiTargetEntry(void* self, std::uint32_t user, std::uint64_t target) noexcept {
  (void)user;
  if (self == nullptr) return;
  TraceSlotCall(self, kOpenPartyUITarget);
  UiEventJob job;
  job.game = GameEvents{};
  job.symbol = 0;
  job.slot = kOpenPartyUITarget;
  job.result = UiEventResult::kNotRun;
  UiEventBegin(self, kOpenPartyUITarget, target, &job);
  RunUiEvent(self, &job);
  UiEventFinish(self, &job);
}

void SlotAcceptInviteEntry(void* self, std::uint32_t index) noexcept {
  if (self == nullptr) return;
  TraceSlotCall(self, kAcceptInvite);
  const std::uint64_t partyId = InvitePartyAt(self, index);
  if (partyId != 0) JoinFlow(self, partyId);
}

}  // namespace quest_social::internal
