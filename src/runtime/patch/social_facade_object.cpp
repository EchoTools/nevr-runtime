#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

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
constexpr std::uint32_t kFriendQueryLogCalls = 64;  // enough to see which rows the tablet reads
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
constexpr std::size_t kMemberJsonSlots = 10;
alignas(16) std::array<std::uint8_t, 16 * kMemberJsonSlots> g_memberJson{};  // zeroed Json slots the game reads per member

// Party data (proposal §3). The game's CJson functions (social_facade.h JsonOps), set once at Install;
// everything below them runs on the game's thread (Update, slot 30, Reset).
JsonOps g_jsonOps;
std::atomic<bool> g_jsonOpsSet{false};
std::array<std::uint64_t, kMemberJsonSlots> g_slotMember{};         // whose server data each slot holds
std::array<SocialParty::JsonText, kMemberJsonSlots> g_slotData{};   // the text loaded there (nullptr: none)
SocialParty::JsonText g_partyDataLoaded;                            // the party data loaded into +0x1F0
bool g_memberDataWritten = false;  // slot 30 handed out the local member's JSON since it was last shared
std::uint64_t g_sharedParty = 0;   // the party the local data was last shared into
std::uint32_t g_partyDataShared = 0;
std::uint32_t g_memberDataShared = 0;
std::string g_lastShared;

std::shared_ptr<const SocialParty::View> CurrentView() {
  std::lock_guard<std::mutex> guard(g_viewMutex);
  return g_view;
}

void PublishView() {
  SocialParty::View view = SocialParty::Global().Snapshot();
  // The game's party UI reads member 0, the local user, whether or not a party exists (it logged
  // "R15NetPartyMember index is out of range (0 >= 0)" and sent no invite when MemberCount was 0);
  // pnsovr's MemberCount starts at the local-member count, so it is at least 1 once the local user
  // is set. Show the local user as the only member until a party replaces the list.
  if (view.members.empty() && view.selfId != 0) {
    SocialParty::Member self;
    self.id = view.selfId;
    self.name = view.selfName.empty() ? std::to_string(view.selfId) : view.selfName;
    view.members.push_back(self);
  }
  auto next = std::make_shared<const SocialParty::View>(std::move(view));
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

/// pnsovr's room id field (+0x2A8): the party, or while a join is in flight the party being joined
/// (JoinInternal stores it before the join answers, 0x18008d3db).
std::uint64_t ViewRoomId(const SocialParty::View& view) { return view.partyId != 0 ? view.partyId : view.joiningPartyId; }

/// The object fields the game reads directly (not through a slot): room id, owner index, the local
/// and total member counts, the creating/joining flag bits, max members and the member JSON array.
void SyncObject(void* self, const SocialParty::View& view) {
  Put64(self, 0x2A8, ViewRoomId(view));
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

const JsonOps* ReadyJsonOps() {
  if (!g_jsonOpsSet.load(std::memory_order_acquire)) return nullptr;
  const JsonOps& ops = g_jsonOps;
  if (ops.load == nullptr || ops.clear == nullptr || ops.serialize == nullptr || ops.blockReset == nullptr ||
      ops.blockDestroy == nullptr)
    return nullptr;
  return &ops;
}

void* MemberJson(std::size_t index) { return g_memberJson.data() + 16 * index; }

/// Loads a JSON object's text into one of the game's CJson (replacing what it held).
bool LoadJson(const JsonOps& ops, void* json, const std::string& text, const char* what, std::uint64_t id) {
  const std::uint32_t result = ops.load(json, text.data(), static_cast<std::int64_t>(text.size()));
  if (result != 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] party data load failed %s id=%llu bytes=%zu result=%u", what,
        static_cast<unsigned long long>(id), text.size(), result);
    return false;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party data loaded %s id=%llu bytes=%zu", what,
      static_cast<unsigned long long>(id), text.size());
  return true;
}

/// The game's own text for one of its CJson, as the game's own callers produce it (Send 0x14060e380):
/// serialise into a CMemBlock, copy the text at [+0] of length [+0x30], then release the block.
bool SerializeJson(const JsonOps& ops, void* json, std::string* out) {
  alignas(16) std::array<std::uint8_t, 0x40> block{};
  ops.serialize(json, block.data(), 0, "");
  const char* text = nullptr;
  std::uint64_t length = 0;
  std::uint32_t blockFlags = 0;
  std::memcpy(&text, block.data(), sizeof(text));
  std::memcpy(&length, block.data() + 0x30, sizeof(length));
  std::memcpy(&blockFlags, block.data() + 0x1C, sizeof(blockFlags));
  out->assign(text != nullptr ? text : "", text != nullptr ? static_cast<std::size_t>(length) : 0);
  if ((blockFlags & 6U) != 0) ops.blockReset(block.data());
  ops.blockDestroy(block.data());
  while (!out->empty() && out->back() == '\0') out->pop_back();
  if (out->find_first_not_of(" \t\r\n") == std::string::npos) *out = "{}";  // the empty document
  const nlohmann::json parsed = nlohmann::json::parse(*out, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] party data not shared: the game's JSON is not an object (%zu bytes)",
        out->size());
    return false;
  }
  return true;
}

/// What LoadReceivedData changed, for the callbacks fired after the events.
struct ReceivedData {
  std::vector<std::uint32_t> members;  // member indexes whose data changed
  bool party = false;
};

/// Loads the server's data that changed since the last frame into the game's JSON: each remote
/// member's into its slot (+0x248, index 0 is the local member's own and is never loaded), and the
/// party's into +0x1F0 for a member (the leader's is its own). A slot whose member changed (a join,
/// a leave shifting the list) is reloaded or cleared, so slot i always holds member i's data.
ReceivedData LoadReceivedData(void* self, const SocialParty::View& view) {
  ReceivedData changed;
  const JsonOps* ops = ReadyJsonOps();
  if (ops == nullptr) return changed;
  for (std::size_t i = 1; i < kMemberJsonSlots; ++i) {
    const SocialParty::Member* member = i < view.members.size() ? &view.members[i] : nullptr;
    const std::uint64_t id = member != nullptr ? member->id : 0;
    const SocialParty::JsonText data = member != nullptr ? member->data : nullptr;
    if (g_slotMember[i] == id && g_slotData[i] == data) continue;
    if (data != nullptr) {
      if (LoadJson(*ops, MemberJson(i), *data, "member", id)) changed.members.push_back(static_cast<std::uint32_t>(i));
    } else if (g_slotData[i] != nullptr) {
      ops->clear(MemberJson(i));
    }
    g_slotMember[i] = id;
    g_slotData[i] = data;
  }
  const bool host = view.partyId != 0 && view.ownerId == view.selfId;
  if (!host && view.partyData != g_partyDataLoaded) {
    if (view.partyData != nullptr)
      changed.party = LoadJson(*ops, Bytes(self) + 0x1F0, *view.partyData, "party", view.partyId);
    else if (g_partyDataLoaded != nullptr)
      ops->clear(Bytes(self) + 0x1F0);
    g_partyDataLoaded = view.partyData;
  }
  return changed;
}

void SendParty(const char* what, const std::vector<SocialParty::Message>& messages);

/// pnsovr's share half of Update (0x1800ac240, slots 7 and 6): the leader's party data when the game
/// marked it written (flags bit 0, which is then cleared), the local member's after slot 30 handed it
/// out; both once more on entering a party, so the server holds them from the start.
void ShareLocalData(void* self, const SocialParty::View& view) {
  const JsonOps* ops = ReadyJsonOps();
  if (ops == nullptr || view.partyId == 0 || view.joining) return;
  const bool newParty = view.partyId != g_sharedParty;
  if (newParty) {
    g_sharedParty = view.partyId;
    g_memberDataWritten = true;
  }
  const std::uint32_t flags = Get32(self, 0x27C);
  std::string text;
  if (view.ownerId == view.selfId && ((flags & 1U) != 0 || newParty)) {
    Put32(self, 0x27C, flags & ~1U);
    if (SerializeJson(*ops, Bytes(self) + 0x1F0, &text)) {
      ++g_partyDataShared;
      g_lastShared = text;
      Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party data share scope=party party=%llu bytes=%zu",
          static_cast<unsigned long long>(view.partyId), text.size());
      SendParty("party data (party)", SocialParty::Global().ShareData(SocialParty::kPartyDataScopeParty, text));
    }
  }
  if (g_memberDataWritten) {
    g_memberDataWritten = false;
    if (SerializeJson(*ops, MemberJson(0), &text)) {
      ++g_memberDataShared;
      g_lastShared = text;
      Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party data share scope=member party=%llu bytes=%zu",
          static_cast<unsigned long long>(view.partyId), text.size());
      SendParty("party data (member)", SocialParty::Global().ShareData(SocialParty::kPartyDataScopeMember, text));
    }
  }
}

/// Releases every JSON the facade or the game put in the party's and members' slots, and forgets what
/// was loaded (Reset). Without the game's clear the bytes are zeroed, as before.
void ClearPartyJson(void* self) {
  const JsonOps* ops = ReadyJsonOps();
  if (ops != nullptr) {
    ops->clear(Bytes(self) + 0x1F0);
    for (std::size_t i = 0; i < kMemberJsonSlots; ++i) ops->clear(MemberJson(i));
  }
  std::memset(Bytes(self) + 0x1F0, 0, 16);
  std::memset(g_memberJson.data(), 0, g_memberJson.size());
  g_slotMember.fill(0);
  for (SocialParty::JsonText& data : g_slotData) data.reset();
  g_partyDataLoaded.reset();
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

/// Once per Update, on the game's thread. Received data is loaded before the events fire, so a
/// MemberJoined callback already finds the member's data (PartyMemberJoinedCB reads its headsettype);
/// pnsovr then fired MemberUpdated for it, and Updated for the party's data.
void PumpParty(void* self) {
  PublishView();
  const auto view = CurrentView();
  SyncObject(self, *view);
  const ReceivedData received = LoadReceivedData(self, *view);
  for (const SocialParty::Event& event : SocialParty::Global().DrainEvents()) DispatchEvent(self, event);
  for (const std::uint32_t index : received.members) CallU32(self, kCbMemberUpdated, index);
  if (received.party) CallVoid(self, kCbUpdated);
}

void SendParty(const char* what, const std::vector<SocialParty::Message>& messages) {
  if (messages.empty()) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] %s: nothing to send in the current party state", what);
    return;
  }
  const bool sent = SocialParty::Send(messages);
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] %s: %zu request(s) %s", what, messages.size(),
      sent ? "sent" : "NOT sent");
}

void SendInvite(void*, std::uint64_t target) { SendParty("party invite", SocialParty::Global().SendInvite(target)); }
void LeaveParty(void*) { SendParty("party leave", SocialParty::Global().Leave()); }
void PassOwnership(void*, std::uint32_t index) { SendParty("party pass", SocialParty::Global().Pass(index)); }
void KickMember(void*, std::uint32_t index) { SendParty("party kick", SocialParty::Global().Kick(index)); }
void DismissInvite(void*, std::int32_t index) {
  SendParty("party invite dismiss", SocialParty::Global().Dismiss(static_cast<std::uint32_t>(index)));
}
// Slot 2 JoinInternal (pnsovr 0x18008d1e0), in pnsovr's order: drop the invites to that party,
// defer if a create or join is in flight, run the accept gate, then leave and join.
void JoinParty(void* self, std::uint64_t partyId) {
  SocialParty::State& party = SocialParty::Global();
  if (!party.BeginJoin(partyId)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party join party=%llu: deferred=%llu (a create or join is in flight)",
        static_cast<unsigned long long>(partyId), static_cast<unsigned long long>(party.DeferredJoin()));
    return;
  }
  if (!CallGate(self, kCbInviteAccepted)) {
    party.AbandonJoin(partyId);
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party join party=%llu: the game declined to join",
        static_cast<unsigned long long>(partyId));
    return;
  }
  SendParty("party join", party.Join(partyId));
}

// Slot 73 AcceptInvite (pnsovr 0x1800821f0): JoinInternal on the party of the invite at `index`.
void AcceptInvite(void* self, std::int32_t index) {
  const std::uint64_t partyId = index < 0 ? 0 : SocialParty::Global().InvitePartyAt(static_cast<std::uint32_t>(index));
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] party accept index=%d party=%llu", index,
      static_cast<unsigned long long>(partyId));
  if (partyId != 0) JoinParty(self, partyId);
}

// The game calls this when the friends tab opens. The roster is otherwise filled only once, at
// login, so a friend added since (for example on the web site) never showed until a restart.
void RefreshFriends(void*) { SendParty("refresh friends", SocialParty::Global().RefreshFriends()); }
// Slot 37 OpenFriendRequestUI: the game's add-friend node (R15NetAddFriendNode, run 0x140dd90f0 ->
// 0x1401870f0) calls it with (0, target account) after its provider checks. pnsovr opened the
// Oculus friend-request overlay; here it is the friend request itself.
void OpenFriendRequestUI(void*, std::uint64_t, std::uint64_t target) {
  SendParty("friend request", SocialParty::Global().RequestFriend(target));
}

// Slot 30 MemberDataWritable (0x18008fcf0): only for the local member (index 0, once a local user
// exists), it marks the data dirty and returns the member's JSON root for the game to write into.
std::uint64_t MemberDataWritable(void* self, std::int32_t index) {
  if (index != 0 || Get32(self, 0x200) == 0) return 0;
  g_memberDataWritten = true;  // shared by the next Update (ShareLocalData)
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(g_memberJson.data()));
}

// Slot 16 SetJoinPolicy: the policy the JoinPolicy slot (21) reports, and, for a party this client
// leads, the server's (SocialParty::SetJoinPolicy).
void SetJoinPolicy(void* self, std::uint32_t policy) {
  Put32(self, 0x2B4, policy);
  SendParty("party join policy", SocialParty::Global().SetJoinPolicy(policy));
}

std::uint32_t Ready(void*) {
  const auto view = CurrentView();
  const std::uint32_t result = view->partyId != 0 && !view->joining ? 1U : 0U;
  LogQuery("Ready", 0xA0, CountCall(g_calls.ready), result);
  return result;
}

// The host's own wish to be joinable: bit 1 of the flags word, set by Reset and flipped by the game's
// PartyLock node (0x140189f20 writes it on the social object).
bool HostWantsJoinable(const void* self) { return ((Get32(self, 0x27C) >> 1) & 1U) != 0; }

// pnsovr's joinable rule (slot 22, 0x18008d430), shared by the Joinable slot and FriendIsInvitable:
// Ready, then for the host its own flag bit 1, for a member the party's server-side lock, and room
// for one more.
bool PartyJoinable(const SocialParty::View& view) {
  if (view.partyId == 0 || view.joining || view.members.size() >= kPartyMaxMembers) return false;
  const bool host = view.ownerId == view.selfId;
  return host ? HostWantsJoinable(&g_object) : !view.locked;
}

std::uint32_t Joinable(void*) {
  const auto view = CurrentView();
  const std::uint32_t result = PartyJoinable(*view) ? 1U : 0U;
  LogQuery("Joinable", 0xB0, CountCall(g_calls.joinable), result);
  return result;
}

std::uint64_t* Host(void*, std::uint64_t* out) {
  const auto view = CurrentView();
  const std::uint64_t result = view->partyId != 0 ? view->ownerId : view->selfId;  // member 0 leads an empty party
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
  const std::uint64_t result = ViewRoomId(*CurrentView());
  LogQuery("Id", 0xC8, CountCall(g_calls.id), result);
  return result;
}

std::uint32_t MemberCount(void*) { return static_cast<std::uint32_t>(CurrentView()->members.size()); }

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
  SendParty("party create (the game asked for a party)", request);
}

// Slot 4 JoinableInternal (pnsovr reads the room's joinable state): the party's server-side lock.
std::uint32_t JoinableInternal(void*) { return CurrentView()->locked ? 0U : 1U; }

// Slot 5 SetJoinableInternal (pnsovr 0x1800926f0, the room's lock/unlock): Lock or Unlock on the server.
void SetJoinableInternal(void*, std::uint32_t joinable) {
  SendParty(joinable != 0 ? "party unlock" : "party lock", SocialParty::Global().SetLocked(joinable == 0));
}

// The host half of pnsovr's per-frame sync (0x1800ac240, the tail of Update): when the host's own
// joinable bit differs from the room's, set the room's (slot 5).
void SyncHostJoinable(void* self) {
  const auto view = CurrentView();
  if (view->partyId == 0 || view->joining || view->ownerId != view->selfId) return;
  const std::uint32_t wanted = HostWantsJoinable(self) ? 1U : 0U;
  if (wanted != JoinableInternal(self)) SetJoinableInternal(self, wanted);
}

void Update(void* self, const void* flags) {
  FlushJsonTraces();
  // pnsovr's Update retries a deferred join in place of its create decision (0x180095ab4).
  const std::uint64_t deferredJoin = SocialParty::Global().DeferredJoin();
  if (deferredJoin != 0)
    JoinParty(self, deferredJoin);
  else
    MaybeCreateParty(flags);
  PumpParty(self);
  ShareLocalData(self, *CurrentView());
  SyncHostJoinable(self);
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
  const std::uint32_t callCount = CountCall(g_calls.friendId);
  if (callCount <= kFriendQueryLogCalls) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade query slot=0x188 name=FriendId index=%u id=%llu call_count=%u",
        index, static_cast<unsigned long long>(id), callCount);
  }
  return out;
}

const char* FriendName(void*, std::uint32_t index) {
  const char* name = SocialRoster::Global().NameAt(index);
  const std::uint32_t callCount = CountCall(g_calls.friendName);
  if (callCount <= kFriendQueryLogCalls) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade query slot=0x190 name=FriendName index=%u text=%s call_count=%u",
        index, name, callCount);
  }
  return name;
}

std::uint32_t FriendStatus(void*, std::uint32_t index) {
  const std::uint32_t result = SocialRoster::Global().OnlineAt(index) ? 2U : 0U;
  LogQuery("FriendStatus", 0x198, CountCall(g_calls.friendStatus), result);
  return result;
}

// pnsovr (0x180084f80): a friend is invitable only while the local party is joinable, the friend is
// not already a member, and the platform lists them as invitable (Oculus's invitable-users map, which
// holds the people it can reach; here that is the friends who are online). It is not a function of
// the friend alone: with no party, or a full or locked one, no row shows the "+" button.
// Slots 52/54/55 (pnsovr 0x1800850e0, 0x180085030, 0x180085060): the friend's presence text, whether
// their party can be joined, and its id, from the server's SNSFriendPresenceNotify.
const char* FriendStatusString(void*, std::uint32_t index) { return SocialRoster::Global().StatusTextAt(index); }
std::uint32_t FriendIsJoinable(void*, std::uint32_t index) { return SocialRoster::Global().PartyIdAt(index) != 0 ? 1U : 0U; }
std::uint64_t FriendPartyId(void*, std::uint32_t index) { return SocialRoster::Global().PartyIdAt(index); }

std::uint32_t FriendIsInvitable(void*, std::uint32_t index) {
  std::uint64_t id = 0;
  if (!SocialRoster::Global().IdAt(index, &id) || !SocialRoster::Global().OnlineAt(index)) return 0;
  const auto view = CurrentView();
  if (!PartyJoinable(*view)) return 0;
  for (const SocialParty::Member& member : view->members)
    if (member.id == id) return 0;
  return 1;
}

// Recently met (slots 56-67, pnsovr 0x180091200.. over Oculus' list; here the server's, §2). The
// refresh node (0x140ddfcc0) calls slot 57 and then polls slot 56 until it reads 0; the
// R15NetRecentlyMetUser(s) expressions (0x140da0e80, 0x140da11f0) and the status node (0x140da5d80,
// which keeps the text up to its first '|') read the rest. Online people come first.
std::uint64_t NowSeconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t RecentlyMetRefreshing(void*) {
  bool timedOut = false;
  const bool busy = SocialRoster::RecentlyMet().Refreshing(NowSeconds(), &timedOut);
  if (timedOut) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.SOCIAL] recently met refresh: no answer within %llu s; the list stays as it was",
        static_cast<unsigned long long>(SocialRoster::RecentList::kRefreshSeconds));
  }
  return busy ? 1U : 0U;
}

void RefreshRecentlyMet(void*) {
  if (!SocialRoster::RecentlyMet().BeginRefresh(NowSeconds())) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] recently met refresh: one is already in flight");
    return;
  }
  const std::vector<SocialParty::Message> request = SocialParty::Global().RefreshRecentlyMet();
  const bool sent = !request.empty() && SocialParty::Send(request);
  if (!sent) SocialRoster::RecentlyMet().EndRefresh();
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] recently met refresh: request %s", sent ? "sent" : "NOT sent");
}

std::uint32_t RecentlyMetCount(void*) { return SocialRoster::RecentlyMet().Count(); }
std::uint32_t RecentlyMetOnlineCount(void*) { return SocialRoster::RecentlyMet().Online(); }
std::uint32_t RecentlyMetOfflineCount(void*) {
  return SocialRoster::RecentlyMet().Count() - SocialRoster::RecentlyMet().Online();
}
std::uint64_t* RecentlyMetUserId(void*, std::uint64_t* out, std::uint32_t index) {
  std::uint64_t id = 0;
  SocialRoster::RecentlyMet().IdAt(index, &id);
  if (out != nullptr) *out = id;
  return out;
}
const char* RecentlyMetUserName(void*, std::int32_t index) {
  return index < 0 ? "" : SocialRoster::RecentlyMet().NameAt(static_cast<std::uint32_t>(index));
}
// pnsovr 0x180090b90: 2 for an index below the online count, else 0.
std::uint32_t RecentlyMetUserStatus(void*, std::uint32_t index) {
  return SocialRoster::RecentlyMet().OnlineAt(index) ? 2U : 0U;
}
const char* RecentlyMetUserStatusString(void*, std::int32_t index) {
  return index < 0 ? "" : SocialRoster::RecentlyMet().TextAt(static_cast<std::uint32_t>(index));
}
// pnsovr 0x180090aa0: only while the local party is joinable (slot 22), someone it can reach (online
// here, as for friends) who is not already a member.
std::uint32_t RecentlyMetUserIsInvitable(void*, std::uint32_t index) {
  std::uint64_t id = 0;
  if (!SocialRoster::RecentlyMet().IdAt(index, &id) || !SocialRoster::RecentlyMet().OnlineAt(index)) return 0;
  const auto view = CurrentView();
  if (!PartyJoinable(*view)) return 0;
  for (const SocialParty::Member& member : view->members)
    if (member.id == id) return 0;
  return 1;
}
std::uint32_t RecentlyMetUserIsJoinable(void*, std::uint32_t index) {
  return SocialRoster::RecentlyMet().PartyIdAt(index) != 0 ? 1U : 0U;
}
std::uint64_t RecentlyMetUserPartyId(void*, std::uint32_t index) { return SocialRoster::RecentlyMet().PartyIdAt(index); }

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

// Base Reset (0x1800ab420): clears the room JSON, sets the state word to (state & ~1) | 2, zeroes both
// member counts and the member JSON, and puts the lobby fields back to "no lobby".
void ResetBase(void* self) {
  ClearPartyJson(self);  // room JSON root and the member JSON
  Put32(self, 0x27C, (Get32(self, 0x27C) & ~1U) | 2U);
  Put32(self, 0x200, 0);
  Put32(self, 0x204, 0);
  const std::uint64_t invalidMatchType = UINT64_MAX;
  const std::uint16_t invalidTeam = UINT16_MAX;
  const std::uint8_t privateLobby = 2;
  std::memcpy(Bytes(self) + 0x270, &invalidMatchType, sizeof(invalidMatchType));
  std::memcpy(Bytes(self) + 0x278, &invalidTeam, sizeof(invalidTeam));
  std::memcpy(Bytes(self) + 0x27A, &privateLobby, sizeof(privateLobby));
}

// Slot 12 Reset (0x180091660): pnsovr leaves the room it is in (no Left callback), clears its caches,
// then runs the base reset. The game follows every call with SetLocalUser.
void Reset(void* self) {
  SendParty("party reset (leave the party)", SocialParty::Global().ResetParty());
  ResetBase(self);
}

// Slot 34 ExitLobby (0x180084a10): only the lobby fields go back to "no lobby" and the offline bit is
// cleared; nothing about the party is touched.
void ExitLobby(void* self) {
  const std::uint64_t invalidMatchType = UINT64_MAX;
  const std::uint16_t invalidTeam = UINT16_MAX;
  const std::uint8_t privateLobby = 2;
  std::memcpy(Bytes(self) + 0x270, &invalidMatchType, sizeof(invalidMatchType));
  std::memcpy(Bytes(self) + 0x278, &invalidTeam, sizeof(invalidTeam));
  std::memcpy(Bytes(self) + 0x27A, &privateLobby, sizeof(privateLobby));
  Put32(self, 0x27C, Get32(self, 0x27C) & ~0x10U);
}

// Slot 31 EnterLobby (0x180084960): stores the lobby fields and sets or clears only the offline bit.
void EnterLobby(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team, std::uint8_t lobbyType,
                int offline) {
  if (uuid != nullptr) std::memcpy(Bytes(self) + 0x260, uuid, 16);
  std::memcpy(Bytes(self) + 0x270, &matchType, sizeof(matchType));
  std::memcpy(Bytes(self) + 0x278, &team, sizeof(team));
  std::memcpy(Bytes(self) + 0x27A, &lobbyType, sizeof(lobbyType));
  const std::uint32_t flags = Get32(self, 0x27C);
  Put32(self, 0x27C, offline != 0 ? (flags | 0x10U) : (flags & ~0x10U));
}

// Slots the facade does not implement still answer (zero). Their calls are logged by the Traced wrapper
// every slot goes through, with the three integer argument registers (the first is `this`; the rest
// may be unused).
std::uint64_t UnimplementedSlot(void*, std::uint64_t, std::uint64_t) { return 0; }

// Slots 32 and 33 are forwarders into EnterLobby in pnsovr (0x1800849e0, 0x1800849b0: they pass their
// registers straight through). The game calls 32 from LobbySessionSuccessCB with (uuid, matchType,
// team, lobbyType) and 33 from LobbyRegistrationSuccessCB with only (uuid, matchType, byte); EnterLobby's
// lobbyType and offline arguments are then whatever was left in the registers and on the stack, so
// 33 stores the three values it was given and leaves lobbyType and the offline bit as they were.
void EnterLobbySession(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team,
                       std::uint8_t lobbyType) {
  EnterLobby(self, uuid, matchType, team, lobbyType, 0);
}

void EnterLobbyRegistration(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team) {
  if (uuid != nullptr) std::memcpy(Bytes(self) + 0x260, uuid, 16);
  std::memcpy(Bytes(self) + 0x270, &matchType, sizeof(matchType));
  std::memcpy(Bytes(self) + 0x278, &team, sizeof(team));
}

// Every slot the game can call goes through Traced: it logs the slot, its name, the call number, the
// argument registers and the result, for the first calls of each slot and then every
// kTraceEvery-th one, so a run shows exactly which parts of the interface the game uses (the ones
// polled every frame do not flood the log).
constexpr std::uint32_t kTraceFirstCalls = 24;
constexpr std::uint32_t kTraceEvery = 600;

const char* const kSlotNames[kRealVtableSlotCount] = {
    "SwapMembers",
    "RemoveMember",
    "JoinInternal",
    "LeaveInternal",
    "JoinableInternal",
    "SetJoinableInternal",
    "PushMemberData",
    "ShareData",
    "SendInvite",
    "Initialize",
    "Shutdown",
    "destructor/release",
    "Reset",
    "Update",
    "SetLocalUser",
    "RemoveLocalMember",
    "SetJoinPolicy",
    "Leave",
    "PassOwnership",
    "Kick",
    "Ready",
    "JoinPolicy",
    "Joinable",
    "Host",
    "IsHost",
    "Id",
    "MemberCount",
    "MemberId",
    "MemberName",
    "MemberVisible",
    "MemberDataWritable",
    "EnterLobby",
    "EnterLobby forwarder",
    "EnterLobby forwarder",
    "ExitLobby",
    "EnterGame",
    "ExitGame",
    "OpenFriendRequestUI",
    "OpenSendInviteUI",
    "OpenNewSendInviteUI(target)",
    "OpenNewSendInviteUI",
    "OpenRecvInviteUI",
    "OpenPartyUI(target)",
    "OpenPartyUI",
    "RefreshingFriends",
    "RefreshFriends",
    "FriendCount",
    "OnlineFriendCount",
    "OfflineFriendCount",
    "FriendId",
    "FriendName",
    "FriendStatus",
    "FriendStatusString",
    "FriendIsInvitable",
    "FriendIsJoinable",
    "FriendPartyId",
    "RefreshingRecentlyMetUsers",
    "RefreshRecentlyMetUsers",
    "RecentlyMetUserCount",
    "OnlineRecentlyMetUserCount",
    "OfflineRecentlyMetUserCount",
    "RecentlyMetUserId",
    "RecentlyMetUserName",
    "RecentlyMetUserStatus",
    "RecentlyMetUserStatusString",
    "RecentlyMetUserIsInvitable",
    "RecentlyMetUserIsJoinable",
    "RecentlyMetUserPartyId",
    "RefreshingInvites",
    "RefreshInvites",
    "InviteCount",
    "InviteSender",
    "InviteSentTime",
    "AcceptInvite",
    "DismissInvite"
};

template <typename T>
std::uint64_t TraceValue(T value) {
  if constexpr (std::is_pointer_v<T>) return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
  else return static_cast<std::uint64_t>(value);
}

template <std::size_t SlotIndex, auto Fn>
struct Traced;

template <std::size_t SlotIndex, typename R, typename... A, R (*Fn)(A...)>
struct Traced<SlotIndex, Fn> {
  static R Call(A... args) {
    static std::atomic<std::uint32_t> calls{0};
    const std::uint32_t count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool log = count <= kTraceFirstCalls || count % kTraceEvery == 0;
    std::uint64_t values[sizeof...(A) + 1] = {TraceValue(args)..., 0};
    if constexpr (std::is_void_v<R>) {
      Fn(args...);
      if (log) {
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] slot=%zu name=%s call=%u args=%llx,%llx,%llx", SlotIndex,
            kSlotNames[SlotIndex], count, static_cast<unsigned long long>(values[0]),
            static_cast<unsigned long long>(sizeof...(A) > 1 ? values[1] : 0),
            static_cast<unsigned long long>(sizeof...(A) > 2 ? values[2] : 0));
      }
    } else {
      const R result = Fn(args...);
      if (log) {
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] slot=%zu name=%s call=%u args=%llx,%llx,%llx result=%llx", SlotIndex,
            kSlotNames[SlotIndex], count, static_cast<unsigned long long>(values[0]),
            static_cast<unsigned long long>(sizeof...(A) > 1 ? values[1] : 0),
            static_cast<unsigned long long>(sizeof...(A) > 2 ? values[2] : 0),
            static_cast<unsigned long long>(TraceValue(result)));
      }
      return result;
    }
  }
};

#define TRACED(slot, fn) reinterpret_cast<Slot>(&Traced<slot, &fn>::Call)

// Slot order is the 75-entry CNSOVRSocial table at pnsovr.dll 0x1801FC2E0.
// Each entry has a signature-compatible empty implementation for its return
// shape; ID wrappers use the Win64 hidden-result-pointer convention.
const std::array<Slot, kVtableSlotCount> kVtable = {
    TRACED(0, VoidU32U32),  // 00 SwapMembers
    TRACED(1, UnimplementedSlot),  // 01 RemoveMember
    TRACED(2, JoinParty),  // 02 JoinInternal
    TRACED(3, UnimplementedSlot),  // 03 LeaveInternal
    TRACED(4, JoinableInternal),  // 04 JoinableInternal
    TRACED(5, SetJoinableInternal),  // 05 SetJoinableInternal
    TRACED(6, UnimplementedSlot),  // 06 PushMemberData
    TRACED(7, UnimplementedSlot),  // 07 ShareData
    TRACED(8, SendInvite),  // 08 SendInvite
    TRACED(9, Initialize),  // 09 Initialize
    TRACED(10, Shutdown),  // 10 Shutdown
    TRACED(11, Void0),  // 11 destructor/release
    TRACED(12, Reset),  // 12 Reset
    TRACED(13, Update),  // 13 Update
    TRACED(14, SetLocalUser),  // 14 SetLocalUser
    TRACED(15, UnimplementedSlot),  // 15 RemoveLocalMember
    TRACED(16, SetJoinPolicy),  // 16 SetJoinPolicy
    TRACED(17, LeaveParty),  // 17 Leave
    TRACED(18, PassOwnership),  // 18 PassOwnership
    TRACED(19, KickMember),  // 19 Kick
    TRACED(20, Ready),  // 20 Ready
    TRACED(21, JoinPolicy),  // 21 JoinPolicy
    TRACED(22, Joinable),  // 22 Joinable
    TRACED(23, Host),  // 23 Host
    TRACED(24, IsHost),  // 24 IsHost
    TRACED(25, Id),  // 25 Id
    TRACED(26, MemberCount),  // 26 MemberCount
    TRACED(27, MemberId),  // 27 MemberId
    TRACED(28, MemberName),  // 28 MemberName
    TRACED(29, ZeroU32),  // 29 MemberVisible
    TRACED(30, MemberDataWritable),  // 30 MemberDataWritable
    TRACED(31, EnterLobby),  // 31 EnterLobby
    TRACED(32, EnterLobbySession),  // 32 EnterLobby forwarder (session success)
    TRACED(33, EnterLobbyRegistration),  // 33 EnterLobby forwarder (registration success)
    TRACED(34, ExitLobby),  // 34 ExitLobby
    TRACED(35, UnimplementedSlot),  // 35 EnterGame
    TRACED(36, UnimplementedSlot),  // 36 ExitGame
    TRACED(37, OpenFriendRequestUI),  // 37 OpenFriendRequestUI
    TRACED(38, UnimplementedSlot),  // 38 OpenSendInviteUI
    TRACED(39, UnimplementedSlot),  // 39 OpenNewSendInviteUI(target): 0x140187230 passes (0, account)
    TRACED(40, UnimplementedSlot),  // 40 OpenNewSendInviteUI: 0x140187330 passes (0)
    TRACED(41, UnimplementedSlot),  // 41 OpenRecvInviteUI
    TRACED(42, UnimplementedSlot),  // 42 OpenPartyUI(target): 0x1401873f0 passes (0, account)
    TRACED(43, UnimplementedSlot),  // 43 OpenPartyUI: 0x1401874f0 passes (0)
    TRACED(44, Zero0),  // 44 RefreshingFriends
    TRACED(45, RefreshFriends),  // 45 RefreshFriends
    TRACED(46, FriendCount),  // 46 FriendCount
    TRACED(47, OnlineFriendCount),  // 47 OnlineFriendCount
    TRACED(48, OfflineFriendCount),  // 48 OfflineFriendCount
    TRACED(49, FriendId),  // 49 FriendId
    TRACED(50, FriendName),  // 50 FriendName
    TRACED(51, FriendStatus),  // 51 FriendStatus
    TRACED(52, FriendStatusString),  // 52 FriendStatusString
    TRACED(53, FriendIsInvitable),  // 53 FriendIsInvitable
    TRACED(54, FriendIsJoinable),  // 54 FriendIsJoinable
    TRACED(55, FriendPartyId),  // 55 FriendPartyId
    TRACED(56, RecentlyMetRefreshing),  // 56 RefreshingRecentlyMetUsers
    TRACED(57, RefreshRecentlyMet),  // 57 RefreshRecentlyMetUsers
    TRACED(58, RecentlyMetCount),  // 58 RecentlyMetUserCount
    TRACED(59, RecentlyMetOnlineCount),  // 59 OnlineRecentlyMetUserCount
    TRACED(60, RecentlyMetOfflineCount),  // 60 OfflineRecentlyMetUserCount
    TRACED(61, RecentlyMetUserId),  // 61 RecentlyMetUserId
    TRACED(62, RecentlyMetUserName),  // 62 RecentlyMetUserName
    TRACED(63, RecentlyMetUserStatus),  // 63 RecentlyMetUserStatus
    TRACED(64, RecentlyMetUserStatusString),  // 64 RecentlyMetUserStatusString
    TRACED(65, RecentlyMetUserIsInvitable),  // 65 RecentlyMetUserIsInvitable
    TRACED(66, RecentlyMetUserIsJoinable),  // 66 RecentlyMetUserIsJoinable
    TRACED(67, RecentlyMetUserPartyId),  // 67 RecentlyMetUserPartyId
    TRACED(68, Zero0),  // 68 RefreshingInvites
    TRACED(69, UnimplementedSlot),  // 69 RefreshInvites
    TRACED(70, InviteCount),  // 70 InviteCount
    TRACED(71, InviteSender),  // 71 InviteSender
    TRACED(72, InviteSentTime),  // 72 InviteSentTime
    TRACED(73, AcceptInvite),  // 73 AcceptInvite
    TRACED(74, DismissInvite),  // 74 DismissInvite
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

void SetJsonOps(const JsonOps& ops) {
  g_jsonOps = ops;
  g_jsonOpsSet.store(true, std::memory_order_release);
}

void* Object() {
  std::call_once(g_objectOnce, [] {
    g_object.vtable = kVtable.data();
    ResetBase(&g_object);
    Put32(&g_object, 0x2B4, 3);  // join policy: the default before the game sets one
  });
  return &g_object;
}

#ifdef NEVR_TEST_HOOKS
std::uint32_t TestInitializeCallCount() { return g_initializeCalls.load(std::memory_order_relaxed); }
std::uint32_t TestShutdownCallCount() { return g_shutdownCalls.load(std::memory_order_relaxed); }
std::uint32_t TestMaxUsers() { return g_maxUsers; }
const void* TestCallbacksSource() { return g_callbacksSource; }
#endif

#ifdef NEVR_SCENARIO_CONTROL
// Scenario-control builds only (docs/design/2026-10-01-social-scenario-harness.md): the state the
// game would read through the slots, computed by the slots' own functions.
std::int32_t FriendInvitableForTest(std::uint64_t friendId) {
  std::uint64_t id = 0;
  for (std::uint32_t index = 0; SocialRoster::Global().IdAt(index, &id); ++index) {
    if (id == friendId) return static_cast<std::int32_t>(FriendIsInvitable(nullptr, index));
  }
  return -1;
}

std::vector<InviteForTest> InvitesForTest() {
  const auto view = CurrentView();
  std::vector<InviteForTest> out;
  for (std::int32_t index = 0;; ++index) {
    const SocialParty::Invite* invite = InviteAt(*view, index);
    if (invite == nullptr) break;
    out.push_back({invite->partyId, invite->senderId});
  }
  return out;
}

std::vector<RecentlyMetForTest> RecentlyMetUsersForTest() {
  std::vector<RecentlyMetForTest> out;
  for (std::uint32_t index = 0; index < RecentlyMetCount(nullptr); ++index) {
    RecentlyMetForTest user;
    RecentlyMetUserId(nullptr, &user.id, index);
    user.name = RecentlyMetUserName(nullptr, static_cast<std::int32_t>(index));
    user.status = RecentlyMetUserStatus(nullptr, index);
    user.text = RecentlyMetUserStatusString(nullptr, static_cast<std::int32_t>(index));
    user.invitable = RecentlyMetUserIsInvitable(nullptr, index) != 0;
    user.joinable = RecentlyMetUserIsJoinable(nullptr, index) != 0;
    user.partyId = RecentlyMetUserPartyId(nullptr, index);
    out.push_back(user);
  }
  return out;
}

bool RecentlyMetRefreshingForTest() { return RecentlyMetRefreshing(nullptr) != 0; }

PartyStateForTest PartyForTest() {
  const auto view = CurrentView();
  PartyStateForTest out;
  out.partyId = view->partyId;
  out.roomId = ViewRoomId(*view);
  out.joining = view->joining;
  out.joinable = PartyJoinable(*view);
  out.locked = view->locked;
  out.joinPolicy = Get32(&g_object, 0x2B4);
  out.shareDirty = (Get32(&g_object, 0x27C) & 1U) != 0;
  for (const SocialParty::Member& member : view->members) {
    out.memberIds.push_back(member.id);
    out.memberData.push_back(member.data != nullptr ? *member.data : std::string());
  }
  out.partyData = view->partyData != nullptr ? *view->partyData : std::string();
  out.partyDataShared = g_partyDataShared;
  out.memberDataShared = g_memberDataShared;
  out.lastShared = g_lastShared;
  return out;
}
#endif

}  // namespace SocialFacade
