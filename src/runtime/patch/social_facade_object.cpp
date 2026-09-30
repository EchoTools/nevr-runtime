#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "abi/echovr.h"
#include "core/logging.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"
#include "runtime/patch/social_facade.h"

namespace SocialFacade {
namespace {

using Slot = std::uintptr_t;

struct alignas(16) FacadeObject {
  const Slot* vtable;
  std::array<std::uint8_t, 0x1E0> callbacks;
  std::array<std::uint8_t, kObjectSize - 0x1E8> state;
};

static_assert(sizeof(FacadeObject) == kObjectSize, "CNSISocial façade size drift");
static_assert(offsetof(FacadeObject, callbacks) == 0x08, "callback table offset drift");
static_assert(offsetof(FacadeObject, state) == 0x1E8, "CNSISocial state offset drift");

FacadeObject g_object{};
std::once_flag g_objectOnce;
std::atomic<std::uint32_t> g_initializeCalls{0};
std::atomic<std::uint32_t> g_shutdownCalls{0};
std::uint32_t g_maxUsers = 0;
const void* g_callbacksSource = nullptr;
std::array<std::atomic<bool>, kVtableSlotCount - kRealVtableSlotCount> g_paddedSlotLogged{};

struct FacadeCallCounts {
  std::atomic<std::uint32_t> update{0};
  std::atomic<std::uint32_t> setLocalUser{0};
  std::atomic<std::uint32_t> ready{0};
  std::atomic<std::uint32_t> joinPolicy{0};
  std::atomic<std::uint32_t> joinable{0};
  std::atomic<std::uint32_t> host{0};
  std::atomic<std::uint32_t> isHost{0};
  std::atomic<std::uint32_t> id{0};
  std::atomic<std::uint32_t> friendCount{0};
  std::atomic<std::uint32_t> friendId{0};
  std::atomic<std::uint32_t> friendName{0};
  std::atomic<std::uint32_t> friendStatus{0};
};

FacadeCallCounts g_calls;

constexpr std::size_t kJsonTraceCapacity = 32;
constexpr std::size_t kJsonTracePathBytes = 48;
constexpr std::size_t kJsonTracePathWords = kJsonTracePathBytes / sizeof(std::uint64_t);
static_assert(sizeof(JsonTraceRecord::path) == kJsonTracePathBytes);

struct JsonTraceSlot {
  std::atomic<std::uint64_t> stamp{0};
  std::atomic<std::uint32_t> kind{0};
  std::atomic<std::uint32_t> callCount{0};
  std::atomic<std::uint32_t> argument{0};
  std::atomic<std::uint64_t> result{0};
  std::atomic<std::uintptr_t> root{0};
  std::atomic<std::uintptr_t> cache{0};
  std::array<std::atomic<std::uint64_t>, kJsonTracePathWords> path{};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);

std::array<JsonTraceSlot, kJsonTraceCapacity> g_jsonTraceRing{};
std::array<std::atomic<std::uint64_t>, kJsonTraceCapacity> g_jsonTraceFlushed{};
std::atomic<std::uint64_t> g_jsonTraceSequence{0};

constexpr std::uint32_t kInitialQueryLogCalls = 3;
constexpr std::uint32_t kUpdateSummaryInterval = 300;

std::uint8_t* Bytes(void* self) { return static_cast<std::uint8_t*>(self); }

std::uint32_t CountCall(std::atomic<std::uint32_t>& counter) {
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint64_t RoomId(const void* self) {
  std::uint64_t result = 0;
  std::memcpy(&result, static_cast<const std::uint8_t*>(self) + 0x2A8, sizeof(result));
  return result;
}

void LogQuery(const char* name, std::uintptr_t slot, std::uint32_t callCount, std::uint64_t result) {
  if (callCount > kInitialQueryLogCalls) return;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SOCIAL] facade query slot=0x%llx name=%s call_count=%u result=%llu",
      static_cast<unsigned long long>(slot), name, callCount, static_cast<unsigned long long>(result));
}

void Void0(void*) {}
void VoidU32(void*, std::uint32_t) {}
void VoidU64(void*, std::uint64_t) {}
void VoidU32U32(void*, std::uint32_t, std::uint32_t) {}
std::uint64_t Zero0(void*) { return 0; }
std::uint64_t ZeroU32(void*, std::uint32_t) { return 0; }
const char* EmptyU32(void*, std::uint32_t) { return ""; }

template <std::size_t SlotIndex>
std::uint64_t PaddedSlot(void*) {
  static_assert(SlotIndex >= kRealVtableSlotCount && SlotIndex < kVtableSlotCount);
  bool expected = false;
  if (g_paddedSlotLogged[SlotIndex - kRealVtableSlotCount].compare_exchange_strong(
          expected, true, std::memory_order_relaxed)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade padded_vtable_slot slot=%llu offset=0x%llx call_count=1",
        static_cast<unsigned long long>(SlotIndex), static_cast<unsigned long long>(SlotIndex * sizeof(Slot)));
  }
  return 0;
}

std::uint64_t* ZeroId(void*, std::uint64_t* out, std::uint32_t) {
  if (out != nullptr) *out = 0;
  return out;
}

std::uint32_t JoinPolicy(void* self) {
  std::uint32_t result = 0;
  std::memcpy(&result, Bytes(self) + 0x2B4, sizeof(result));
  LogQuery("JoinPolicy", 0xA8, CountCall(g_calls.joinPolicy), result);
  return result;
}

// ---------------------------------------------------------------------------------------------
// Party. The game's script nodes poll these slots and a few object fields every frame and react to
// 15 callbacks the game handed over in Initialize (echovr.exe 0x140173820). The state lives in
// SocialParty::Global(); Update mirrors it into the object and turns each change into the matching
// callback on the game's own thread. Slot meanings are pnsovr's CNSOVRSocial (see the ReVault
// comments on its vtable at pnsovr.dll 0x1801FC2E0).
// ---------------------------------------------------------------------------------------------
constexpr std::size_t kCallbackStride = 0x20;  // context(8) + inline buffer(16) + function(8)
enum Callback : std::size_t {
  kCbCreated = 0, kCbJoined = 1, kCbJoinFailed = 2, kCbUpdated = 3, kCbHostChanged = 4, kCbLeft = 5,
  kCbKicked = 6, kCbInviteAccepted = 7, kCbMemberJoined = 9, kCbMemberUpdated = 10,
  kCbMemberLeft = 11, kCbInviteReceived = 14,
};
constexpr std::uint32_t kPartyMaxMembers = 4;
constexpr std::size_t kViewRing = 128;  // a name pointer the game reads stays valid this many frames

struct CallbackEntry {
  void* context = nullptr;
  void* buffer = nullptr;
  void* function = nullptr;
};

CallbackEntry Entry(void* self, std::size_t index) {
  CallbackEntry entry;
  std::uint8_t* base = Bytes(self) + 8 + kCallbackStride * index;
  std::memcpy(&entry.context, base, sizeof(void*));
  entry.buffer = base + 8;
  std::memcpy(&entry.function, base + 0x18, sizeof(void*));
  return entry;
}

void CallVoid(void* self, std::size_t index) {
  const CallbackEntry e = Entry(self, index);
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party callback index=%zu bound=%d", index, e.function != nullptr ? 1 : 0);
  if (e.function != nullptr) reinterpret_cast<void (*)(void*, void*)>(e.function)(e.context, e.buffer);
}

void CallU32(void* self, std::size_t index, std::uint32_t value) {
  const CallbackEntry e = Entry(self, index);
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party callback index=%zu arg=%u bound=%d", index, value,
      e.function != nullptr ? 1 : 0);
  if (e.function != nullptr) reinterpret_cast<void (*)(void*, void*, std::uint32_t)>(e.function)(e.context, e.buffer, value);
}

void CallIdName(void* self, std::size_t index, std::uint64_t id, const char* name) {
  const CallbackEntry e = Entry(self, index);
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party callback index=%zu id=%llu bound=%d", index,
      static_cast<unsigned long long>(id), e.function != nullptr ? 1 : 0);
  if (e.function != nullptr)
    reinterpret_cast<void (*)(void*, void*, std::uint64_t, const char*)>(e.function)(e.context, e.buffer, id, name);
}

/// PartyInviteAcceptedCB is a gate: the game returns nonzero to allow the join.
bool CallGate(void* self, std::size_t index) {
  const CallbackEntry e = Entry(self, index);
  if (e.function == nullptr) return true;
  const std::uint64_t result = reinterpret_cast<std::uint64_t (*)(void*, void*)>(e.function)(e.context, e.buffer);
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party callback index=%zu gate=%llu", index,
      static_cast<unsigned long long>(result & 0xFFFFFFFFULL));
  return (result & 0xFFFFFFFFULL) != 0;
}

std::mutex g_viewMutex;
std::shared_ptr<const SocialParty::View> g_view = std::make_shared<const SocialParty::View>();
std::array<std::shared_ptr<const SocialParty::View>, kViewRing> g_retiredViews{};
std::size_t g_retiredNext = 0;
alignas(16) std::array<std::uint8_t, 16 * 10> g_memberJson{};  // zeroed Json slots the game reads per member

std::shared_ptr<const SocialParty::View> CurrentView() {
  std::lock_guard<std::mutex> guard(g_viewMutex);
  return g_view;
}

void PublishView() {
  auto next = std::make_shared<const SocialParty::View>(SocialParty::Global().Snapshot());
  std::lock_guard<std::mutex> guard(g_viewMutex);
  g_retiredViews[g_retiredNext] = g_view;
  g_retiredNext = (g_retiredNext + 1) % g_retiredViews.size();
  g_view = std::move(next);
}

std::uint32_t Get32(const void* self, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, static_cast<const std::uint8_t*>(self) + offset, sizeof(value));
  return value;
}
void Put32(void* self, std::size_t offset, std::uint32_t value) { std::memcpy(Bytes(self) + offset, &value, sizeof(value)); }
void Put64(void* self, std::size_t offset, std::uint64_t value) { std::memcpy(Bytes(self) + offset, &value, sizeof(value)); }

/// The object fields the game reads directly (not through a slot): room id, owner index, the local
/// and total member counts, the creating/joining flag bits, max members and the member JSON array.
void SyncObject(void* self, const SocialParty::View& view) {
  Put64(self, 0x2A8, view.partyId);
  std::uint32_t owner = 0;
  for (std::size_t i = 0; i < view.members.size(); ++i)
    if (view.members[i].id == view.ownerId) owner = static_cast<std::uint32_t>(i);
  Put32(self, 0x2B0, owner);
  if (view.selfId != 0) {
    Put32(self, 0x200, 1);
    Put32(self, 0x204, view.members.empty() ? 1U : static_cast<std::uint32_t>(view.members.size()));
  }
  std::uint32_t flags = Get32(self, 0x27C);
  flags = view.creating ? (flags | 4U) : (flags & ~4U);
  flags = view.joining ? (flags | 8U) : (flags & ~8U);
  Put32(self, 0x27C, flags);
  Put32(self, 0x250, kPartyMaxMembers);
  const std::uintptr_t json = reinterpret_cast<std::uintptr_t>(g_memberJson.data());
  std::memcpy(Bytes(self) + 0x248, &json, sizeof(json));
}

void DispatchEvent(void* self, const SocialParty::Event& event) {
  using Kind = SocialParty::EventKind;
  switch (event.kind) {
    case Kind::kCreated: CallVoid(self, kCbCreated); break;
    case Kind::kJoined: CallVoid(self, kCbJoined); break;
    case Kind::kJoinFailed: CallU32(self, kCbJoinFailed, event.code); break;
    case Kind::kUpdated: CallVoid(self, kCbUpdated); break;
    case Kind::kHostChanged: CallVoid(self, kCbHostChanged); break;
    case Kind::kLeft: CallVoid(self, kCbLeft); break;
    case Kind::kKicked: CallVoid(self, kCbKicked); break;
    case Kind::kMemberJoined: CallU32(self, kCbMemberJoined, event.index); break;
    case Kind::kMemberUpdated: CallU32(self, kCbMemberUpdated, event.index); break;
    case Kind::kMemberLeft: CallIdName(self, kCbMemberLeft, event.id, event.name.c_str()); break;
    case Kind::kInviteReceived: CallU32(self, kCbInviteReceived, 0); break;
    case Kind::kInviteFailed: break;  // no game callback is driven from here
  }
}

/// Once per Update, on the game's thread.
void PumpParty(void* self) {
  PublishView();
  SyncObject(self, *CurrentView());
  for (const SocialParty::Event& event : SocialParty::Global().DrainEvents()) DispatchEvent(self, event);
}

void SendParty(const char* what, const std::vector<SocialParty::Message>& messages) {
  if (messages.empty()) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party %s: nothing to send in the current party state", what);
    return;
  }
  const bool sent = SocialParty::Send(messages);
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party %s: %zu request(s) %s", what, messages.size(),
      sent ? "sent" : "NOT sent");
}

void SendInvite(void*, std::uint64_t target) { SendParty("invite", SocialParty::Global().SendInvite(target)); }
void LeaveParty(void*) { SendParty("leave", SocialParty::Global().Leave()); }
void PassOwnership(void*, std::uint32_t index) { SendParty("pass", SocialParty::Global().Pass(index)); }
void KickMember(void*, std::uint32_t index) { SendParty("kick", SocialParty::Global().Kick(index)); }
void DismissInvite(void*, std::int32_t index) {
  SendParty("dismiss", SocialParty::Global().Dismiss(static_cast<std::uint32_t>(index)));
}
void AcceptInvite(void* self, std::int32_t index) {
  if (!CallGate(self, kCbInviteAccepted)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party accept: the game declined to join");
    return;
  }
  SendParty("accept", SocialParty::Global().Accept(static_cast<std::uint32_t>(index)));
}

void SetJoinPolicy(void* self, std::uint32_t policy) { Put32(self, 0x2B4, policy); }

std::uint32_t Ready(void*) {
  const auto view = CurrentView();
  const std::uint32_t result = view->partyId != 0 && !view->joining ? 1U : 0U;
  LogQuery("Ready", 0xA0, CountCall(g_calls.ready), result);
  return result;
}

std::uint32_t Joinable(void*) {
  const auto view = CurrentView();
  const std::uint32_t result =
      view->partyId != 0 && !view->joining && !view->locked && view->members.size() < kPartyMaxMembers ? 1U : 0U;
  LogQuery("Joinable", 0xB0, CountCall(g_calls.joinable), result);
  return result;
}

std::uint64_t* Host(void*, std::uint64_t* out) {
  const std::uint64_t result = CurrentView()->ownerId;
  if (out != nullptr) *out = result;
  LogQuery("Host", 0xB8, CountCall(g_calls.host), result);
  return out;
}

std::uint32_t IsHost(void*) {
  const auto view = CurrentView();
  const bool ready = view->partyId != 0 && !view->joining;
  CountCall(g_calls.isHost);
  return ready && view->ownerId != view->selfId ? 0U : 1U;
}

std::uint64_t Id(void*) {
  const std::uint64_t result = CurrentView()->partyId;
  LogQuery("Id", 0xC8, CountCall(g_calls.id), result);
  return result;
}

std::uint32_t MemberCount(void*) {
  const auto view = CurrentView();
  return view->partyId != 0 ? static_cast<std::uint32_t>(view->members.size()) : 0U;
}

std::uint64_t* MemberId(void*, std::uint64_t* out, std::uint32_t index) {
  const auto view = CurrentView();
  if (out != nullptr) *out = index < view->members.size() ? view->members[index].id : 0;
  return out;
}

const char* MemberName(void*, std::uint32_t index) {
  const auto view = CurrentView();
  return index < view->members.size() ? view->members[index].name.c_str() : "";
}

std::uint32_t InviteCount(void*) { return static_cast<std::uint32_t>(CurrentView()->invites.size()); }

const SocialParty::Invite* InviteAt(const SocialParty::View& view, std::int32_t index) {
  if (index < 0 || static_cast<std::size_t>(index) >= view.invites.size()) return nullptr;
  return &view.invites[view.invites.size() - 1 - static_cast<std::size_t>(index)];  // newest first
}

const char* InviteSender(void*, std::int32_t index) {
  const auto view = CurrentView();
  const SocialParty::Invite* invite = InviteAt(*view, index);
  return invite != nullptr ? invite->senderName.c_str() : "";
}

std::uint64_t InviteSentTime(void*, std::int32_t index) {
  const auto view = CurrentView();
  const SocialParty::Invite* invite = InviteAt(*view, index);
  return invite != nullptr ? invite->sentTime : 0;
}

// Update's second argument points at a flags byte the game fills in: bit 0 asks for a party to exist
// (pnsovr creates its room from Update on that bit, at most every four seconds), bit 1 asks for the
// invite tokens to be refreshed. The game only invites people once a party exists, so without the
// create there is nothing to invite into and no invite ever reaches SendInvite.
constexpr std::uint8_t kUpdateWantsParty = 1;
constexpr std::chrono::seconds kCreateRetryInterval{4};

void MaybeCreateParty(const void* flagsPointer) {
  static std::atomic<std::int32_t> lastFlags{-1};
  static std::chrono::steady_clock::time_point lastCreate{};
  if (flagsPointer == nullptr) return;
  const std::uint8_t flags = *static_cast<const std::uint8_t*>(flagsPointer);
  if (lastFlags.exchange(flags, std::memory_order_relaxed) != flags) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade update flags=0x%02x", flags);
  }
  if ((flags & kUpdateWantsParty) == 0) return;
  const auto now = std::chrono::steady_clock::now();
  if (lastCreate.time_since_epoch().count() != 0 && now - lastCreate < kCreateRetryInterval) return;
  const std::vector<SocialParty::Message> request = SocialParty::Global().CreateParty();
  if (request.empty()) return;
  lastCreate = now;
  SendParty("create (the game asked for a party)", request);
}

void Update(void* self, const void* flags) {
  FlushJsonTraces();
  MaybeCreateParty(flags);
  PumpParty(self);
  const std::uint32_t callCount = CountCall(g_calls.update);
  if (callCount == 1) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade update call_count=1");
  }
  if (callCount % kUpdateSummaryInterval == 0) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SOCIAL] facade calls update=%u set_local_user=%u ready=%u join_policy=%u "
        "joinable=%u host=%u is_host=%u id=%u period=%u",
        callCount, g_calls.setLocalUser.load(std::memory_order_relaxed),
        g_calls.ready.load(std::memory_order_relaxed), g_calls.joinPolicy.load(std::memory_order_relaxed),
        g_calls.joinable.load(std::memory_order_relaxed), g_calls.host.load(std::memory_order_relaxed),
        g_calls.isHost.load(std::memory_order_relaxed), g_calls.id.load(std::memory_order_relaxed),
        kUpdateSummaryInterval);
  }
}

void SetLocalUser(void* self, std::uint32_t userIndex) {
  const std::uint32_t callCount = CountCall(g_calls.setLocalUser);
  if (callCount == 1) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade set_local_user user_index=%u call_count=1", userIndex);
  }
  if (userIndex == 0) {
    Put32(self, 0x200, 1);
    Put32(self, 0x204, 1);
  }
}

// Friend roster slots (44-55). The semantics are the ones pnsovr's CNSOVRSocial implements:
// the count and online count are plain reads, the offline count is count - online, friends are
// ordered online-first, and FriendStatus(i) is 2 for an online friend and 0 for an offline one.
// An online friend can be invited. No friend is joinable or in a party yet.
std::uint32_t FriendCount(void*) {
  const std::uint32_t result = SocialRoster::Global().Count();
  LogQuery("FriendCount", 0x170, CountCall(g_calls.friendCount), result);
  return result;
}

std::uint32_t OnlineFriendCount(void*) { return SocialRoster::Global().Online(); }

std::uint32_t OfflineFriendCount(void*) { return SocialRoster::Global().Offline(); }

std::uint64_t* FriendId(void*, std::uint64_t* out, std::uint32_t index) {
  std::uint64_t id = 0;
  SocialRoster::Global().IdAt(index, &id);
  if (out != nullptr) *out = id;
  LogQuery("FriendId", 0x188, CountCall(g_calls.friendId), id);
  return out;
}

const char* FriendName(void*, std::uint32_t index) {
  const char* name = SocialRoster::Global().NameAt(index);
  LogQuery("FriendName", 0x190, CountCall(g_calls.friendName), name[0] != '\0' ? 1U : 0U);
  return name;
}

std::uint32_t FriendStatus(void*, std::uint32_t index) {
  const std::uint32_t result = SocialRoster::Global().OnlineAt(index) ? 2U : 0U;
  LogQuery("FriendStatus", 0x198, CountCall(g_calls.friendStatus), result);
  return result;
}

std::uint32_t FriendIsInvitable(void*, std::uint32_t index) {
  return SocialRoster::Global().OnlineAt(index) ? 1U : 0U;
}

std::uint64_t Initialize(void* self, std::uint32_t maxUsers, const void* callbacks) {
  auto* object = static_cast<FacadeObject*>(self);
  if (callbacks != nullptr) {
    std::memcpy(object->callbacks.data(), callbacks, object->callbacks.size());
  } else {
    object->callbacks.fill(0);
  }
  g_maxUsers = maxUsers;
  g_callbacksSource = callbacks;
  const std::uint32_t callCount = g_initializeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade initialize max_users=%u callbacks=%p call_count=%u", maxUsers,
      callbacks, callCount);
  return 0;
}

void Shutdown(void* self) {
  auto* object = static_cast<FacadeObject*>(self);
  object->callbacks.fill(0);
  const std::uint32_t callCount = g_shutdownCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade shutdown object=%p call_count=%u lifetime=process", self,
      callCount);
}

void Reset(void* self) {
  std::memset(Bytes(self) + 0x1E8, 0, kObjectSize - 0x1E8);
  const std::uint64_t invalidMatchType = UINT64_MAX;
  const std::uint16_t invalidTeam = UINT16_MAX;
  const std::uint8_t privateLobby = 2;
  const std::uint32_t flags = 2;
  const std::uint32_t joinPolicy = 3;
  std::memcpy(Bytes(self) + 0x270, &invalidMatchType, sizeof(invalidMatchType));
  std::memcpy(Bytes(self) + 0x278, &invalidTeam, sizeof(invalidTeam));
  std::memcpy(Bytes(self) + 0x27A, &privateLobby, sizeof(privateLobby));
  std::memcpy(Bytes(self) + 0x27C, &flags, sizeof(flags));
  std::memcpy(Bytes(self) + 0x2B4, &joinPolicy, sizeof(joinPolicy));
}

void EnterLobby(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team, std::uint8_t lobbyType,
                int offline) {
  if (uuid != nullptr) std::memcpy(Bytes(self) + 0x260, uuid, 16);
  std::memcpy(Bytes(self) + 0x270, &matchType, sizeof(matchType));
  std::memcpy(Bytes(self) + 0x278, &team, sizeof(team));
  std::memcpy(Bytes(self) + 0x27A, &lobbyType, sizeof(lobbyType));
  std::uint32_t flags = 2 | (offline != 0 ? 0x10U : 0U);
  std::memcpy(Bytes(self) + 0x27C, &flags, sizeof(flags));
}

// Slots the facade does not implement still answer, and say so the first few times the game calls
// one, with the three integer argument registers (the pointer is `this`; the rest may be unused).
template <std::size_t SlotIndex>
std::uint64_t LoggedStub(void*, std::uint64_t a, std::uint64_t b) {
  static std::atomic<std::uint32_t> calls{0};
  const std::uint32_t count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 3) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade stub slot=%zu offset=0x%zx call_count=%u args=%llx,%llx",
        SlotIndex, SlotIndex * sizeof(Slot), count, static_cast<unsigned long long>(a),
        static_cast<unsigned long long>(b));
  }
  return 0;
}

// Slots 32 and 33 are forwarders into EnterLobby in pnsovr (they pass their own registers through).
// The game calls 32 from LobbySessionSuccessCB with (uuid, matchType, team, lobbyType) and 33 from
// LobbyRegistrationSuccessCB with (uuid, matchType, byte); the byte lands in the team field in
// pnsovr and its meaning is undetermined, so 33 records only the lobby uuid and match type.
void EnterLobbySession(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team,
                       std::uint8_t lobbyType) {
  EnterLobby(self, uuid, matchType, team, lobbyType, 0);
}

void EnterLobbyRegistration(void* self, const void* uuid, std::uint64_t matchType) {
  if (uuid != nullptr) std::memcpy(Bytes(self) + 0x260, uuid, 16);
  std::memcpy(Bytes(self) + 0x270, &matchType, sizeof(matchType));
}

// Slot order is the 75-entry CNSOVRSocial table at pnsovr.dll 0x1801FC2E0.
// Each entry has a signature-compatible empty implementation for its return
// shape; ID wrappers use the Win64 hidden-result-pointer convention.
const std::array<Slot, kVtableSlotCount> kVtable = {
    reinterpret_cast<Slot>(&VoidU32U32),  // 00 SwapMembers
    reinterpret_cast<Slot>(&LoggedStub<1>),     // 01 RemoveMember
    reinterpret_cast<Slot>(&LoggedStub<2>),       // 02 JoinInternal
    reinterpret_cast<Slot>(&LoggedStub<3>),       // 03 LeaveInternal
    reinterpret_cast<Slot>(&LoggedStub<4>),       // 04 JoinableInternal
    reinterpret_cast<Slot>(&LoggedStub<5>),     // 05 SetJoinableInternal
    reinterpret_cast<Slot>(&LoggedStub<6>),       // 06 PushMemberData
    reinterpret_cast<Slot>(&LoggedStub<7>),     // 07 ShareData
    reinterpret_cast<Slot>(&SendInvite),     // 08 SendInvite
    reinterpret_cast<Slot>(&Initialize),  // 09 Initialize
    reinterpret_cast<Slot>(&Shutdown),    // 10 Shutdown
    reinterpret_cast<Slot>(&Void0),       // 11 destructor/release
    reinterpret_cast<Slot>(&Reset),       // 12 Reset
    reinterpret_cast<Slot>(&Update),      // 13 Update
    reinterpret_cast<Slot>(&SetLocalUser),  // 14 SetLocalUser
    reinterpret_cast<Slot>(&LoggedStub<15>),     // 15 RemoveLocalMember
    reinterpret_cast<Slot>(&SetJoinPolicy),     // 16 SetJoinPolicy
    reinterpret_cast<Slot>(&LeaveParty),       // 17 Leave
    reinterpret_cast<Slot>(&PassOwnership),     // 18 PassOwnership
    reinterpret_cast<Slot>(&KickMember),     // 19 Kick
    reinterpret_cast<Slot>(&Ready),       // 20 Ready
    reinterpret_cast<Slot>(&JoinPolicy),  // 21 JoinPolicy
    reinterpret_cast<Slot>(&Joinable),    // 22 Joinable
    reinterpret_cast<Slot>(&Host),        // 23 Host
    reinterpret_cast<Slot>(&IsHost),      // 24 IsHost
    reinterpret_cast<Slot>(&Id),          // 25 Id
    reinterpret_cast<Slot>(&MemberCount),       // 26 MemberCount
    reinterpret_cast<Slot>(&MemberId),      // 27 MemberId
    reinterpret_cast<Slot>(&MemberName),    // 28 MemberName
    reinterpret_cast<Slot>(&ZeroU32),     // 29 MemberVisible
    reinterpret_cast<Slot>(&ZeroU32),     // 30 MemberDataWritable
    reinterpret_cast<Slot>(&EnterLobby),  // 31 EnterLobby
    reinterpret_cast<Slot>(&EnterLobbySession),     // 32 EnterLobby forwarder (session success)
    reinterpret_cast<Slot>(&EnterLobbyRegistration),     // 33 EnterLobby forwarder (registration success)
    reinterpret_cast<Slot>(&Reset),       // 34 ExitLobby
    reinterpret_cast<Slot>(&LoggedStub<35>),       // 35 EnterGame
    reinterpret_cast<Slot>(&LoggedStub<36>),       // 36 ExitGame
    reinterpret_cast<Slot>(&LoggedStub<37>),       // 37 OpenFriendRequestUI
    reinterpret_cast<Slot>(&LoggedStub<38>),       // 38 OpenSendInviteUI
    reinterpret_cast<Slot>(&LoggedStub<39>),     // 39 OpenNewSendInviteUI
    reinterpret_cast<Slot>(&LoggedStub<40>),     // 40 OpenNewSendInviteUI(target)
    reinterpret_cast<Slot>(&LoggedStub<41>),       // 41 OpenRecvInviteUI
    reinterpret_cast<Slot>(&LoggedStub<42>),     // 42 OpenPartyUI
    reinterpret_cast<Slot>(&LoggedStub<43>),     // 43 OpenPartyUI(target)
    reinterpret_cast<Slot>(&Zero0),       // 44 RefreshingFriends
    reinterpret_cast<Slot>(&LoggedStub<45>),       // 45 RefreshFriends
    reinterpret_cast<Slot>(&FriendCount),  // 46 FriendCount
    reinterpret_cast<Slot>(&OnlineFriendCount),  // 47 OnlineFriendCount
    reinterpret_cast<Slot>(&OfflineFriendCount),  // 48 OfflineFriendCount
    reinterpret_cast<Slot>(&FriendId),     // 49 FriendId
    reinterpret_cast<Slot>(&FriendName),   // 50 FriendName
    reinterpret_cast<Slot>(&FriendStatus), // 51 FriendStatus
    reinterpret_cast<Slot>(&EmptyU32),    // 52 FriendStatusString
    reinterpret_cast<Slot>(&FriendIsInvitable),  // 53 FriendIsInvitable
    reinterpret_cast<Slot>(&ZeroU32),     // 54 FriendIsJoinable
    reinterpret_cast<Slot>(&ZeroU32),     // 55 FriendPartyId
    reinterpret_cast<Slot>(&Zero0),       // 56 RefreshingRecentlyMetUsers
    reinterpret_cast<Slot>(&LoggedStub<57>),       // 57 RefreshRecentlyMetUsers
    reinterpret_cast<Slot>(&Zero0),       // 58 RecentlyMetUserCount
    reinterpret_cast<Slot>(&Zero0),       // 59 OnlineRecentlyMetUserCount
    reinterpret_cast<Slot>(&Zero0),       // 60 OfflineRecentlyMetUserCount
    reinterpret_cast<Slot>(&ZeroId),      // 61 RecentlyMetUserId
    reinterpret_cast<Slot>(&EmptyU32),    // 62 RecentlyMetUserName
    reinterpret_cast<Slot>(&ZeroU32),     // 63 RecentlyMetUserStatus
    reinterpret_cast<Slot>(&EmptyU32),    // 64 RecentlyMetUserStatusString
    reinterpret_cast<Slot>(&ZeroU32),     // 65 RecentlyMetUserIsInvitable
    reinterpret_cast<Slot>(&ZeroU32),     // 66 RecentlyMetUserIsJoinable
    reinterpret_cast<Slot>(&ZeroU32),     // 67 RecentlyMetUserPartyId
    reinterpret_cast<Slot>(&Zero0),       // 68 RefreshingInvites
    reinterpret_cast<Slot>(&LoggedStub<69>),       // 69 RefreshInvites
    reinterpret_cast<Slot>(&InviteCount),       // 70 InviteCount
    reinterpret_cast<Slot>(&InviteSender),    // 71 InviteSender
    reinterpret_cast<Slot>(&InviteSentTime),     // 72 InviteSentTime
    reinterpret_cast<Slot>(&AcceptInvite),     // 73 AcceptInvite
    reinterpret_cast<Slot>(&DismissInvite),     // 74 DismissInvite
    reinterpret_cast<Slot>(&PaddedSlot<75>),  // 75 guard
    reinterpret_cast<Slot>(&PaddedSlot<76>),  // 76 observed DMO-only dispatch at +0x260
    reinterpret_cast<Slot>(&PaddedSlot<77>),  // 77 guard
    reinterpret_cast<Slot>(&PaddedSlot<78>),  // 78 guard
    reinterpret_cast<Slot>(&PaddedSlot<79>),  // 79 guard
    reinterpret_cast<Slot>(&PaddedSlot<80>),  // 80 guard
    reinterpret_cast<Slot>(&PaddedSlot<81>),  // 81 guard
    reinterpret_cast<Slot>(&PaddedSlot<82>),  // 82 guard
    reinterpret_cast<Slot>(&PaddedSlot<83>),  // 83 guard
    reinterpret_cast<Slot>(&PaddedSlot<84>),  // 84 guard
};

static_assert(kVtable.size() >= kRealVtableSlotCount);
static_assert(kVtable.size() >= kMaxObservedGameVtableSlot + 1 + kVtableGuardSlotCount);

}  // namespace

void* Select(bool enabled, void* original) {
  return enabled && original == nullptr ? Object() : original;
}

void QueueJsonTrace(JsonTraceKind kind, std::uint32_t callCount, const char* path,
                    std::uint32_t argument, std::uint64_t result, std::uintptr_t root,
                    std::uintptr_t cache) {
  // The game logger reaches mutexes, heap-backed formatting, and synchronous I/O.
  // JSON hooks publish only atomic fixed-size fields; Update flushes them later.
  const std::uint64_t sequence = g_jsonTraceSequence.fetch_add(1, std::memory_order_relaxed) + 1;
  JsonTraceSlot& slot = g_jsonTraceRing[(sequence - 1) % g_jsonTraceRing.size()];
  std::uint64_t previous = slot.stamp.load(std::memory_order_relaxed);
  if ((previous & 1U) != 0 ||
      !slot.stamp.compare_exchange_strong(previous, sequence * 2 - 1, std::memory_order_acq_rel)) {
    return;
  }

  char pathCopy[kJsonTracePathBytes] = {};
  if (path != nullptr) {
    std::size_t i = 0;
    for (; i + 1 < sizeof(pathCopy) && path[i] != '\0'; ++i) pathCopy[i] = path[i];
    pathCopy[i] = '\0';
  }
  slot.kind.store(static_cast<std::uint32_t>(kind), std::memory_order_relaxed);
  slot.callCount.store(callCount, std::memory_order_relaxed);
  slot.argument.store(argument, std::memory_order_relaxed);
  slot.result.store(result, std::memory_order_relaxed);
  slot.root.store(root, std::memory_order_relaxed);
  slot.cache.store(cache, std::memory_order_relaxed);
  for (std::size_t i = 0; i < slot.path.size(); ++i) {
    std::uint64_t word = 0;
    std::memcpy(&word, pathCopy + i * sizeof(word), sizeof(word));
    slot.path[i].store(word, std::memory_order_relaxed);
  }
  slot.stamp.store(sequence * 2, std::memory_order_release);
}

void DrainJsonTraces(JsonTraceSink sink, void* context) {
  if (sink == nullptr) return;
  for (std::size_t slotIndex = 0; slotIndex < g_jsonTraceRing.size(); ++slotIndex) {
    JsonTraceSlot& slot = g_jsonTraceRing[slotIndex];
    const std::uint64_t stampBefore = slot.stamp.load(std::memory_order_acquire);
    if (stampBefore == 0 || (stampBefore & 1U) != 0) continue;
    const std::uint64_t sequence = stampBefore / 2;
    std::uint64_t flushed = g_jsonTraceFlushed[slotIndex].load(std::memory_order_relaxed);
    if (sequence <= flushed) continue;

    JsonTraceRecord record{};
    record.kind = static_cast<JsonTraceKind>(slot.kind.load(std::memory_order_relaxed));
    record.callCount = slot.callCount.load(std::memory_order_relaxed);
    record.argument = slot.argument.load(std::memory_order_relaxed);
    record.result = slot.result.load(std::memory_order_relaxed);
    record.root = slot.root.load(std::memory_order_relaxed);
    record.cache = slot.cache.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < slot.path.size(); ++i) {
      const std::uint64_t word = slot.path[i].load(std::memory_order_relaxed);
      std::memcpy(record.path + i * sizeof(word), &word, sizeof(word));
    }
    if (slot.stamp.load(std::memory_order_acquire) != stampBefore ||
        !g_jsonTraceFlushed[slotIndex].compare_exchange_strong(flushed, sequence, std::memory_order_relaxed)) {
      continue;
    }

    sink(record, context);
  }
}

void FlushJsonTraces() {
  DrainJsonTraces([](const JsonTraceRecord& record, void*) {
    const char* traceName = "social_json";
    if (record.kind == JsonTraceKind::kSet) traceName = "json_set";
    if (record.kind == JsonTraceKind::kNavigateForWrite) traceName = "json_navigate_write";
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SOCIAL] %s call_count=%u path=%s argument=%u result=0x%llx root=%p cache=%p",
        traceName, record.callCount, record.path[0] != '\0' ? record.path : "<none>", record.argument,
        static_cast<unsigned long long>(record.result), reinterpret_cast<void*>(record.root),
        reinterpret_cast<void*>(record.cache));
  }, nullptr);
}

void* Object() {
  std::call_once(g_objectOnce, [] {
    g_object.vtable = kVtable.data();
    Reset(&g_object);
  });
  return &g_object;
}

#ifdef NEVR_TEST_HOOKS
std::uint32_t TestInitializeCallCount() { return g_initializeCalls.load(std::memory_order_relaxed); }
std::uint32_t TestShutdownCallCount() { return g_shutdownCalls.load(std::memory_order_relaxed); }
std::uint32_t TestMaxUsers() { return g_maxUsers; }
const void* TestCallbacksSource() { return g_callbacksSource; }
#endif

}  // namespace SocialFacade
