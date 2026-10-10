#include "quest/social/social_facade.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "hook_log.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_internal.h"
#include "quest/social/social_request_log.h"

namespace quest_social {
namespace {

using SlotWord = std::uintptr_t;
using sentinel::LogFields;
using sentinel::LogLevel;

constexpr std::size_t kViewRing = 128;  // a name pointer the game reads stays valid this many Updates
// Every call is counted in Impl::slotCalls (Facade::SlotCalls). A slot's first kTraceFirstCalls calls are
// logged with their number, so a button's slot shows each press ("slot=46 call=7"); after that a slot logs
// at most one line per kTraceGapSeconds, carrying the running number. The per-frame slots (counts and
// getters the UI polls) are called many times a second, so they settle at one line per gap each; a slot
// that was quiet for a gap logs its next call at once.
constexpr std::uint32_t kTraceFirstCalls = 8;
constexpr std::uint64_t kTraceGapSeconds = 10;
// CNSOVRSocial::Update retries a failed create no sooner than 5 s after the last one: it compares whole
// seconds (CSysTime::GetTick / GetTicksPerSecond) against the time stored at +0x340 with `cmp w8, #5; b.lo`
// (libpnsovr 0x2045dc..0x2045f8). The lock retry uses the same interval.
constexpr std::uint64_t kCreateRetrySeconds = 5;
constexpr std::uint64_t kLockRetrySeconds = 5;
// A create, join or lock request the sender took and the server never answers is treated as refused after
// this long. How it was chosen: Nakama answers a create at once, success or failure (snsPartyCreateRequest),
// and answers a join at once too, except that a join to a LOCKED party queues for the leader's approval with
// no reply at all (snsPartyJoinRequest); so silence is a normal state for a join and must end in the game
// being told. 10 s is twice the longest wait the game's own social code applies (the 5 s create retry
// above), so a slow reply is not mistaken for none. A reply that arrives after the deadline is still
// applied by the model (a late PartyJoinSuccess admits the player as usual). Not measured against a live
// server's reply times.
constexpr std::uint64_t kPendingDeadlineSeconds = 10;
constexpr std::uint8_t kUpdateWantsParty = 1;  // Update's flags byte, bit 0: a party should exist
// The carry queue. Past kSoftQueueLimit the oldest coalescible event (one a later event of its kind makes
// redundant) is dropped to make room; events that carry information no later event repeats (Created,
// Joined, JoinFailed, HostChanged, Left, Kicked, MemberJoined, MemberLeft) are never dropped for room, so
// the queue may grow past the soft limit up to kHardQueueLimit, where the newest is dropped and counted.
constexpr std::size_t kSoftQueueLimit = 256;
constexpr std::size_t kHardQueueLimit = 4096;

std::atomic<std::uint64_t> g_membersHidden{0};
std::atomic<std::uint64_t> g_eventsDropped{0};
std::atomic<std::uint64_t> g_sendFailed{0};
std::atomic<std::uint64_t> g_joinDeferred{0};
std::atomic<std::uint64_t> g_requestTimeout{0};
// The game's CJson functions (SetGameJson), read on the game's thread.
std::atomic<std::uintptr_t> g_pnsovrBias{0};
std::atomic<CJsonResetFn> g_cjsonReset{nullptr};
std::atomic<CJsonDecodeFromFn> g_cjsonDecode{nullptr};
std::atomic<CJsonEncodeToCompactFn> g_cjsonEncode{nullptr};
// Callbacks delivered to the game, by class (the reporter thread logs them; nothing logs on the delivery).
std::atomic<std::uint64_t> g_cbCreated{0};
std::atomic<std::uint64_t> g_cbMemberJoined{0};
std::atomic<std::uint64_t> g_cbJoinFailed{0};
std::atomic<std::uint64_t> g_cbOther{0};
std::atomic<std::uint64_t> g_jsonFailed{0};     // party or member data the game's JSON would not load, or could not be read out
std::atomic<std::uint64_t> g_framesIgnored{0};  // server frames of a social kind that changed nothing (NoteFrameIgnored)

GameJson CurrentGameJson() {
  GameJson json;
  json.reset = g_cjsonReset.load(std::memory_order_acquire);
  json.decode = g_cjsonDecode.load(std::memory_order_acquire);
  json.encode = g_cjsonEncode.load(std::memory_order_acquire);
  return json;
}

void Count(std::atomic<std::uint64_t>& counter, std::uint64_t n = 1) noexcept {
  counter.fetch_add(n, std::memory_order_relaxed);
}

}  // namespace

static std::atomic<std::uint32_t> g_destroyed{0};

struct Facade::Impl {
  Ports ports;
  alignas(16) std::array<std::uint8_t, kObjectSize> object{};
  std::array<SlotWord, kSlotCount> vtable{};
  alignas(16) std::array<std::uint8_t, 16 * kMemberJsonSlots> memberJson{};  // zeroed CJson, the empty document

  std::mutex viewMutex;
  std::shared_ptr<const nevr_social_party::View> view = std::make_shared<const nevr_social_party::View>();
  std::array<std::shared_ptr<const nevr_social_party::View>, kViewRing> retired{};
  std::size_t retiredNext = 0;

  std::array<std::atomic<std::uint32_t>, kSlotCount> slotCalls{};
  std::array<std::atomic<std::uint64_t>, kSlotCount> slotTraceAt{};  // Now() of each slot's last logged call
  std::atomic<std::uint32_t> initializeCalls{0};
  std::atomic<std::uint32_t> shutdownCalls{0};
  std::atomic<std::uint32_t> slotFailures{0};
  std::atomic<std::uint32_t> callbackCalls{0};

  std::deque<nevr_social_party::Event> queuedEvents;  // drained from the model, not yet delivered (game thread only)
  bool queueOverflowLogged = false;
  std::uint64_t deferredLoggedParty = 0;  // the party whose deferral was last logged (0: none)
  // Party members past the game's array (kMemberJsonSlots): in the model, never announced to the game.
  std::set<std::uint64_t> hiddenMembers;
  std::uint64_t hiddenLoggedParty = 0;
  // Party and member data: whose server data each member JSON slot holds (slot 0, the local member's, never
  // holds the server's), the text loaded there, and the party data loaded into +0x1F0.
  std::array<std::uint64_t, kMemberJsonSlots> slotMember{};
  std::array<nevr_social_party::JsonText, kMemberJsonSlots> slotData{};
  nevr_social_party::JsonText partyDataLoaded;
  std::atomic<bool> memberDataWritten{false};  // slot 31 handed out the local member's JSON since it was last shared
  std::uint64_t sharedParty = 0;               // the party the local data was last shared into
  bool jsonUnavailableLogged = false;
  std::array<char, internal::kShareBufferBytes> partyBuffer{};
  std::array<char, internal::kShareBufferBytes> memberBuffer{};
  // When the sender took a request that has no answer yet (0: none); see kPendingDeadlineSeconds.
  std::uint64_t createSince = 0;
  std::uint64_t joinSince = 0;
  std::uint64_t lockSince = 0;
  bool lockWantLocked = false;  // what the lock request in flight (or last refused) asked for
  bool lockTried = false;
  std::uint64_t lastLock = 0;

  bool processWide = false;  // the Instance(): its destruction is a defect the test pins
  bool createTried = false;
  std::uint64_t lastCreate = 0;
  int lastUpdateFlags = -1;
};

namespace {

using Impl = Facade::Impl;

// ---- object access --------------------------------------------------------------------------

std::uint8_t* Bytes(void* self) { return static_cast<std::uint8_t*>(self); }
const std::uint8_t* Bytes(const void* self) { return static_cast<const std::uint8_t*>(self); }

template <typename T>
T Get(const void* self, std::size_t offset) {
  T value{};
  std::memcpy(&value, Bytes(self) + offset, sizeof(value));
  return value;
}

template <typename T>
void Put(void* self, std::size_t offset, T value) {
  std::memcpy(Bytes(self) + offset, &value, sizeof(value));
}

Impl* OwnerOf(void* self) noexcept {
  if (self == nullptr) return nullptr;
  return Get<Impl*>(self, kOffOwner);
}

std::uint64_t Now(const Impl& impl) {
  if (impl.ports.nowSeconds != nullptr) return impl.ports.nowSeconds();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- model access ---------------------------------------------------------------------------

nevr_social_party::State& Party(Impl& impl) { return *impl.ports.party; }
nevr_social_roster::Roster& Friends(Impl& impl) { return *impl.ports.friends; }
nevr_social_roster::RecentList& Recent(Impl& impl) { return *impl.ports.recent; }

std::shared_ptr<const nevr_social_party::View> CurrentView(Impl& impl) {
  std::lock_guard<std::mutex> guard(impl.viewMutex);
  return impl.view;
}

// Publishes the model for the game and returns every member's id in model order. The game indexes a member
// JSON array of kMemberJsonSlots entries by the member count this object reports and by the index each
// member callback carries (libr15 PartyMemberData 0x129b3fc, PartyMemberHeadsetType 0x129b168,
// PartyMemberJoinedCB 0x126f8ac), with no check against anything but slot 27, so the game is shown the
// first kMemberJsonSlots members and no more; the rest stay in the model, hidden (see QueueEvents).
std::vector<std::uint64_t> PublishView(Impl& impl) {
  nevr_social_party::View next = Party(impl).Snapshot();
  // The game's party UI reads member 0, the local user, whether or not a party exists, so the local
  // user is the only member until a party replaces the list (CNSOVRSocial counts the local member too).
  if (next.members.empty() && next.selfId != 0) {
    nevr_social_party::Member self;
    self.id = next.selfId;
    self.name = next.selfName.empty() ? std::to_string(next.selfId) : next.selfName;
    next.members.push_back(self);
  }
  std::vector<std::uint64_t> ids;
  ids.reserve(next.members.size());
  for (const nevr_social_party::Member& member : next.members) ids.push_back(member.id);
  if (next.members.size() > kMemberJsonSlots) next.members.resize(kMemberJsonSlots);
  auto shared = std::make_shared<const nevr_social_party::View>(std::move(next));
  std::lock_guard<std::mutex> guard(impl.viewMutex);
  impl.retired[impl.retiredNext] = impl.view;
  impl.retiredNext = (impl.retiredNext + 1) % impl.retired.size();
  impl.view = std::move(shared);
  return ids;
}

std::uint64_t ViewRoomId(const nevr_social_party::View& view) {
  return view.partyId != 0 ? view.partyId : view.joiningPartyId;
}

// ---- sending --------------------------------------------------------------------------------

// Sends the requests and logs one line per request with its stable ids (social_request_log.h). Returns true
// only if the sender took all of them; nothing to send is true, and logs one line saying so. A create the
// sender took starts the clock on its answer (kPendingDeadlineSeconds).
bool SendParty(Impl& impl, const char* what, const std::vector<nevr_social_party::Message>& messages) {
  if (messages.empty()) {
    LogFields(LogLevel::kInfo, "social_send", {{"what", what}, {"count", 0}, {"sent", "nothing_to_send"}});
    return true;
  }
  const bool sent = impl.ports.send != nullptr && impl.ports.send(messages);
  if (!sent) Count(g_sendFailed);
  LogRequests(what, messages, sent, ViewRoomId(*CurrentView(impl)));
  if (sent) {
    for (const nevr_social_party::Message& m : messages) {
      if (m.symbol == nevr_social_party::kCreateRequest) impl.createSince = Now(impl);
    }
  }
  return sent;
}

// The lock or unlock the host's joinable bit asks for. `retry` is the automatic retry from Update, which waits
// kLockRetrySeconds after the last attempt at the same state; a call from the game (slot 5) goes at once. A
// refused send rolls the model back so the next attempt can send it.
void RequestLock(Impl& impl, bool wantLocked, bool retry) {
  const std::uint64_t now = Now(impl);
  if (retry && impl.lockTried && impl.lockWantLocked == wantLocked && now >= impl.lastLock &&
      now - impl.lastLock < kLockRetrySeconds) {
    return;
  }
  const std::vector<nevr_social_party::Message> request = Party(impl).SetLocked(wantLocked);
  if (request.empty()) return;  // already asked and not yet answered (or no party)
  impl.lockTried = true;
  impl.lastLock = now;
  impl.lockWantLocked = wantLocked;
  if (SendParty(impl, wantLocked ? "party lock" : "party unlock", request)) {
    impl.lockSince = now;
  } else {
    Party(impl).ForgetLockRequest();
  }
}

// A create, join or lock request the sender took and the server never answered is failed after
// kPendingDeadlineSeconds exactly like a refused send: the model rolls back (the join also gives the invites
// back and tells the game, State::AbandonJoining), the failure is counted and logged once, and the usual
// retry applies (the create by Update's decision, the lock by Update's backoff; a join is retried by the player
// from the JoinFailed the game was told).
bool Overdue(std::uint64_t since, std::uint64_t now) {
  return since != 0 && now >= since && now - since >= kPendingDeadlineSeconds;
}

void ExpirePending(Impl& impl) {
  const std::uint64_t now = Now(impl);
  nevr_social_party::State& party = Party(impl);
  if (Overdue(impl.createSince, now)) {
    impl.createSince = 0;
    if (party.AbandonCreate()) {
      Count(g_requestTimeout);
      LogFields(LogLevel::kWarn, "social_request_timeout",
                {{"what", "party create"}, {"seconds", static_cast<long long>(kPendingDeadlineSeconds)}, {"action", "rolled_back"}});
    }
  }
  if (Overdue(impl.joinSince, now)) {
    impl.joinSince = 0;
    const std::uint64_t joining = CurrentView(impl)->joiningPartyId;
    if (party.AbandonJoining()) {
      Count(g_requestTimeout);
      LogFields(LogLevel::kWarn, "social_request_timeout",
                {{"what", "party join"}, {"party", static_cast<long long>(joining)},
                 {"seconds", static_cast<long long>(kPendingDeadlineSeconds)}, {"action", "join_failed_to_game"}});
    }
  }
  if (Overdue(impl.lockSince, now)) {
    impl.lockSince = 0;
    if (party.ExpireLockRequest(impl.lockWantLocked)) {
      Count(g_requestTimeout);
      LogFields(LogLevel::kWarn, "social_request_timeout",
                {{"what", impl.lockWantLocked ? "party lock" : "party unlock"},
                 {"seconds", static_cast<long long>(kPendingDeadlineSeconds)}, {"action", "rolled_back"}});
    }
  }
}

// ---- party and member data ------------------------------------------------------------------

// What the game's JSON must load from the server's data (nevr_social_party::State::ReceiveData keeps it): each remote member's
// data into its slot of the member array (+0x248; slot 0 is the local member's own and is never loaded) and the party's
// into +0x1F0 for a member (the leader's is its own). A slot whose member changed (a join, a leave shifting the list)
// is reloaded or cleared, so slot i always holds member i's data. The texts are the strings the view shares and the
// facade keeps in slotData / partyDataLoaded until the next plan. Without the game's functions nothing is planned and
// nothing is marked loaded, so the data loads once they are known.
void PlanReceivedData(Impl& impl, const nevr_social_party::View& view, internal::JsonPlan* plan) {
  plan->game = CurrentGameJson();
  plan->count = 0;
  const bool canLoad = plan->game.decode != nullptr && plan->game.reset != nullptr;
  const auto add = [plan](internal::JsonOpKind kind, std::uint8_t slot, const std::string* text) {
    internal::JsonOp& op = plan->ops[plan->count++];
    op.kind = kind;
    op.slot = slot;
    op.ok = 0;
    op.text = text != nullptr ? text->data() : nullptr;
    op.length = text != nullptr ? text->size() : 0;
  };
  bool pending = false;
  for (std::size_t i = 1; i < kMemberJsonSlots; ++i) {
    const nevr_social_party::Member* member = i < view.members.size() ? &view.members[i] : nullptr;
    const std::uint64_t id = member != nullptr ? member->id : 0;
    const nevr_social_party::JsonText data = member != nullptr ? member->data : nullptr;
    if (impl.slotMember[i] == id && impl.slotData[i] == data) continue;
    if (!canLoad) {
      pending = pending || data != nullptr || impl.slotData[i] != nullptr;
      continue;
    }
    if (data != nullptr) {
      add(internal::kJsonLoad, static_cast<std::uint8_t>(i), data.get());
    } else if (impl.slotData[i] != nullptr) {
      add(internal::kJsonClear, static_cast<std::uint8_t>(i), nullptr);
    }
    impl.slotMember[i] = id;
    impl.slotData[i] = data;
  }
  const bool host = view.partyId != 0 && view.ownerId == view.selfId;
  if (!host && view.partyData != impl.partyDataLoaded) {
    if (!canLoad) {
      pending = true;
    } else {
      if (view.partyData != nullptr) {
        add(internal::kJsonLoad, internal::kJsonParty, view.partyData.get());
      } else if (impl.partyDataLoaded != nullptr) {
        add(internal::kJsonClear, internal::kJsonParty, nullptr);
      }
      impl.partyDataLoaded = view.partyData;
    }
  }
  if (pending && !impl.jsonUnavailableLogged) {
    impl.jsonUnavailableLogged = true;
    Count(g_jsonFailed);
    LogFields(LogLevel::kWarn, "social_party_data",
              {{"result", "game_json_unavailable"}, {"note", "server party or member data is held until the game's CJson functions are known"}});
  }
}

// ---- object fields the game reads directly --------------------------------------------------

// "No lobby": the invalid uuid, no match type, no team, the private lobby type. NRadEngine::SUuid::kInvalid is
// sixteen zero bytes: libr15 defines it in .bss (0x376c3b8, 16 bytes, so zero at load; social_pinned_test checks
// the section) and the only write through its GOT entry (0x372adc8) is its own initialiser,
// CMemory::Fill(&kInvalid, 0, 16) at 0xf54c4c..0xf54c58. The other 136 GOT loads only read it: copies of its
// value, CMemory::Compare arguments, const-reference calls. No direct (non-GOT) reference exists. CNSISocial::
// Reset (libr15 0x1919854) and ExitLobby (pnsovr 0x2084d0) both copy it to +0x260.
void ResetLobbyFields(void* self) {
  std::memset(Bytes(self) + kOffLobbyUuid, 0, 16);
  Put<std::uint64_t>(self, kOffLobbyMatchType, UINT64_MAX);
  Put<std::uint16_t>(self, kOffLobbyTeam, UINT16_MAX);
  Put<std::uint8_t>(self, kOffLobbyType, 2);
}

void SyncObject(Impl& impl, const nevr_social_party::View& view) {
  void* self = impl.object.data();
  Put<std::uint64_t>(self, kOffRoomId, ViewRoomId(view));
  std::uint32_t owner = 0;
  for (std::size_t i = 0; i < view.members.size(); ++i) {
    if (view.members[i].id == view.ownerId) owner = static_cast<std::uint32_t>(i);
  }
  Put<std::uint32_t>(self, kOffOwnerIndex, owner);
  if (view.selfId != 0) {
    Put<std::uint32_t>(self, kOffLocalCount, 1);
    Put<std::uint32_t>(self, kOffMemberCount, view.members.empty() ? 1U : static_cast<std::uint32_t>(view.members.size()));
  }
  std::uint32_t flags = Get<std::uint32_t>(self, kOffFlags);
  flags = view.creating ? (flags | kFlagCreating) : (flags & ~kFlagCreating);
  flags = view.joining ? (flags | kFlagJoining) : (flags & ~kFlagJoining);
  Put<std::uint32_t>(self, kOffFlags, flags);
  Put<std::uint32_t>(self, kOffMaxMembers, kPartyMaxMembers);
  const std::uintptr_t json = reinterpret_cast<std::uintptr_t>(impl.memberJson.data());
  Put<std::uintptr_t>(self, kOffMemberJson, json);
}

bool HostWantsJoinable(const Impl& impl) {
  return (Get<std::uint32_t>(impl.object.data(), kOffFlags) & kFlagJoinable) != 0;
}

// The joinable rule of CNSOVRSocial::Joinable (libpnsovr 0x205180, shared with FriendIsInvitable):
// Ready, then for the host its own flag, for a member the party's server-side lock, and room for one more.
bool PartyJoinable(const Impl& impl, const nevr_social_party::View& view) {
  if (view.partyId == 0 || view.joining || view.members.size() >= kPartyMaxMembers) return false;
  const bool host = view.ownerId == view.selfId;
  return host ? HostWantsJoinable(impl) : !view.locked;
}

// ---- slots ----------------------------------------------------------------------------------
// Each takes the object as its first argument and returns what CNSOVRSocial's method returns under
// AAPCS64 (ids are 64-bit values in x0; there is no hidden result pointer on this ABI).

void SlotNothing(void*) {}
void SlotNothingU32(void*, std::uint32_t) {}
void SlotNothingU32U32(void*, std::uint32_t, std::uint32_t) {}
void SlotNothingPtr(void*, const void*) {}
std::uint32_t SlotZero32(void*) { return 0; }

void SlotSendInvite(void* self, std::uint64_t target) {
  Impl& impl = *OwnerOf(self);
  if (!SendParty(impl, "party invite", Party(impl).SendInvite(target))) Party(impl).AbandonCreate();
}

void SlotLeave(void* self) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, "party leave", Party(impl).Leave());
}

void SlotPassOwnership(void* self, std::uint32_t index) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, "party pass", Party(impl).Pass(index));
}

void SlotKick(void* self, std::uint32_t index) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, "party kick", Party(impl).Kick(index));
}

void SlotDismissInvite(void* self, std::uint32_t index) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, "party invite dismiss", Party(impl).Dismiss(index));
}

void SlotRefreshFriends(void* self) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, "refresh friends", Party(impl).RefreshFriendsOnTabOpen(Now(impl)));
}

// OpenFriendRequestUI(LocalUserID, UserAccountID): pnsovr opened the Oculus friend-request overlay; the
// NEVR friend request is the request itself.
void SlotOpenFriendRequestUI(void* self, std::uint32_t, std::uint64_t target) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, "friend request", Party(impl).RequestFriend(target));
}

void SlotSetJoinPolicy(void* self, std::uint32_t policy) {
  Impl& impl = *OwnerOf(self);
  Put<std::uint32_t>(self, kOffJoinPolicy, policy);
  SendParty(impl, "party join policy", Party(impl).SetJoinPolicy(policy));
}

std::uint32_t SlotJoinPolicy(void* self) { return Get<std::uint32_t>(self, kOffJoinPolicy); }

std::uint32_t SlotReady(void* self) {
  const auto view = CurrentView(*OwnerOf(self));
  return view->partyId != 0 && !view->joining ? 1U : 0U;
}

std::uint32_t SlotJoinable(void* self) {
  Impl& impl = *OwnerOf(self);
  return PartyJoinable(impl, *CurrentView(impl)) ? 1U : 0U;
}

std::uint64_t SlotHost(void* self) {
  const auto view = CurrentView(*OwnerOf(self));
  return view->partyId != 0 ? view->ownerId : view->selfId;  // member 0 leads an empty party
}

std::uint32_t SlotIsHost(void* self) {
  const auto view = CurrentView(*OwnerOf(self));
  const bool ready = view->partyId != 0 && !view->joining;
  return ready && view->ownerId != view->selfId ? 0U : 1U;
}

std::uint64_t SlotId(void* self) { return ViewRoomId(*CurrentView(*OwnerOf(self))); }

// The count the object reports: the party's members, at least the local-user count AddMember wrote (so
// the engine's MemberCount - [+0x200] is never negative before the first Update), never more than the
// game's member JSON array holds.
std::uint32_t ReportedMemberCount(Impl& impl) {
  std::uint32_t count = static_cast<std::uint32_t>(CurrentView(impl)->members.size());
  const std::uint32_t local = Get<std::uint32_t>(impl.object.data(), kOffLocalCount);
  if (count < local) count = local;
  return count > kMemberJsonSlots ? kMemberJsonSlots : count;
}

std::uint32_t SlotMemberCount(void* self) { return ReportedMemberCount(*OwnerOf(self)); }

std::uint64_t SlotMemberId(void* self, std::uint32_t index) {
  Impl& impl = *OwnerOf(self);
  const auto view = CurrentView(impl);
  return index < view->members.size() && index < kMemberJsonSlots ? view->members[index].id : 0;
}

const char* SlotMemberName(void* self, std::uint32_t index) {
  Impl& impl = *OwnerOf(self);
  const auto view = CurrentView(impl);
  return index < view->members.size() && index < kMemberJsonSlots ? view->members[index].name.c_str() : "";
}

// CNSOVRSocial::LocalId (libpnsovr 0x20527c): member 0 is local user 0, anyone else is no local user.
// CNSOVRSocial::LocalId (libpnsovr 0x20527c, `mov w8, #-1; csel x0, xzr, x8, eq`): the invalid local user id is the
// 32-bit -1 zero-extended, not 64 bits of ones.
std::uint64_t SlotLocalId(void*, std::uint32_t index) { return index == 0 ? 0 : 0xFFFFFFFFULL; }

// MemberDataWritable hands out the local member's CJson (the first of the member array) for the game to write its
// per-member data into (the headset type), and notes that it has to be shared; null is what CNSOVRSocial answers for
// every other member and before a local user exists (libpnsovr 0x205308). The game's Update shares it (ShareBegin).
std::uint64_t SlotMemberDataWritable(void* self, std::uint32_t index) {
  Impl& impl = *OwnerOf(self);
  if (index != 0 || Get<std::uint32_t>(self, kOffLocalCount) == 0) return 0;
  impl.memberDataWritten.store(true, std::memory_order_relaxed);  // shared by the next Update (ShareBegin)
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(impl.memberJson.data()));
}

std::uint32_t SlotJoinableInternal(void* self) { return CurrentView(*OwnerOf(self))->locked ? 0U : 1U; }

void SlotSetJoinableInternal(void* self, std::uint32_t joinable) {
  RequestLock(*OwnerOf(self), joinable == 0, false);
}

// Once, when the game takes the facade: the provider symbol pnsovr registered, the platform code the game derives from it
// for friend ids (CR15NetGame::FriendId) and the code the login carries. If they differ every friend row would carry
// another platform than the one the server keys accounts by, and the game drops such rows silently.
void LogProviderIdentity() {
  const std::uintptr_t bias = g_pnsovrBias.load(std::memory_order_acquire);
  if (bias == 0) {
    LogFields(LogLevel::kInfo, "social_provider", {{"result", "pnsovr_not_selected"}});
    return;
  }
  std::uint64_t symbol = 0;
  std::memcpy(&symbol, reinterpret_cast<const void*>(bias + static_cast<std::uintptr_t>(kPnsovrProviderSymbolVaddr)),
              sizeof(symbol));
  const bool ovr = symbol == kProviderSymbolOvr;
  char hex[19];
  LogFields(ovr ? LogLevel::kInfo : LogLevel::kWarn, "social_provider",
            {{"symbol", sentinel::HexString(hex, symbol)},
             {"game_platform_code", ovr ? static_cast<long long>(kPlatformCodeOvr) : 0LL},
             {"login_platform_code", static_cast<long long>(kPlatformCodeOvr)},
             {"match", ovr ? "yes" : "NO_friend_rows_would_be_dropped"}});
}

std::int32_t SlotInitialize(void* self, std::uint32_t maxUsers, const void* callbacks) {
  Impl& impl = *OwnerOf(self);
  if (callbacks != nullptr) {
    std::memcpy(Bytes(self) + 8, callbacks, kCallbackBytes);
  } else {
    std::memset(Bytes(self) + 8, 0, kCallbackBytes);
  }
  const std::uint32_t calls = impl.initializeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  LogFields(LogLevel::kInfo, "social_initialize",
            {{"max_users", maxUsers}, {"callbacks", callbacks != nullptr ? "given" : "null"}, {"calls", calls}});
  if (calls == 1) LogProviderIdentity();
  return 0;
}

void SlotShutdown(void* self) {
  Impl& impl = *OwnerOf(self);
  std::memset(Bytes(self) + 8, 0, kCallbackBytes);  // no callback may reach a game object that is going away
  const std::uint32_t calls = impl.shutdownCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  LogFields(LogLevel::kInfo, "social_shutdown", {{"calls", calls}});
}

// The object lives as long as the process: neither destructor slot may free it.
void SlotDestructor(void*) {
  LogFields(LogLevel::kWarn, "social_destructor", {{"action", "ignored_process_lifetime"}});
}

// The base reset (libpnsovr 0x1800ab420's twin): clear the member data, put the state word to
// (state & ~1) | 2, zero both member counts and put the lobby fields back to "no lobby".
//
// +0x1F0 is the party CJson. The social object owns it (the native constructor builds it, pnsovr 0x203254,
// the destructor destroys it, 0x20845c) and the game fills it while this client leads a party
// (CR15NetGame::Update, libr15 0x12951d4..0x1295254). The native Reset clears it with CJson::Reset (pnsovr
// 0x36a92c): SlotResetEntry does the same with the game's own function, outside this exception-enabled file
// (social_game_calls.cpp). Zeroing the pointer here would leak the tree; leaving it would keep the old
// party's lobby settings, which the game reads when it is not the host (PartyTeam libr15 0x129a9ac, PartyData
// 0x129aa08). The constructor path (ResetBase before any game call) finds it zero, which CJson::Reset accepts.
//
// The member JSON array (+0x248) holds the local member's data the game wrote and the server's data for the others:
// trees the game allocated. With the game's CJson::Reset known, SlotResetEntry frees them (so they are not zeroed
// here); without it nothing was ever loaded into them, and they are zeroed.
void ResetBase(Impl& impl, bool gameFreesJson) {
  void* self = impl.object.data();
  if (!gameFreesJson) std::memset(impl.memberJson.data(), 0, impl.memberJson.size());
  Put<std::uint32_t>(self, kOffFlags, (Get<std::uint32_t>(self, kOffFlags) & ~kFlagDataWritten) | kFlagJoinable);
  Put<std::uint32_t>(self, kOffLocalCount, 0);
  Put<std::uint32_t>(self, kOffMemberCount, 0);
  ResetLobbyFields(self);
}

void SlotAddMember(void* self, std::uint32_t userIndex) {
  if (userIndex != 0) return;
  Put<std::uint32_t>(self, kOffLocalCount, 1);
  Put<std::uint32_t>(self, kOffMemberCount, 1);
}

void MaybeCreateParty(Impl& impl, const void* flagsPointer) {
  if (flagsPointer == nullptr) return;
  const std::uint8_t flags = *static_cast<const std::uint8_t*>(flagsPointer);
  if (impl.lastUpdateFlags != flags) {
    impl.lastUpdateFlags = flags;
    LogFields(LogLevel::kInfo, "social_update_flags", {{"flags", flags}});
  }
  if ((flags & kUpdateWantsParty) == 0) return;
  // No signed-in account yet (the login has not finished): a create would carry a null user uuid.
  if (Party(impl).Snapshot().selfId == 0) return;
  const std::uint64_t now = Now(impl);
  if (impl.createTried && now - impl.lastCreate < kCreateRetrySeconds) return;
  const std::vector<nevr_social_party::Message> request = Party(impl).CreateParty();
  if (request.empty()) return;
  impl.createTried = true;
  impl.lastCreate = now;
  // A create that could not be sent is not in flight: put the model back so the retry after the
  // interval can send it (the model set "creating" when it built the request).
  if (!SendParty(impl, "party create", request)) Party(impl).AbandonCreate();
}

void SyncHostJoinable(Impl& impl) {
  const auto view = CurrentView(impl);
  if (view->partyId == 0 || view->joining || view->ownerId != view->selfId) return;
  const std::uint32_t wanted = HostWantsJoinable(impl) ? 1U : 0U;
  const std::uint32_t current = view->locked ? 0U : 1U;
  if (wanted != current) RequestLock(impl, wanted == 0, true);
}

void EnterLobbyFields(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team, std::uint8_t lobbyType,
                      bool offline) {
  const std::uint32_t flags = Get<std::uint32_t>(self, kOffFlags);
  if (uuid != nullptr) std::memcpy(Bytes(self) + kOffLobbyUuid, uuid, 16);
  Put<std::uint64_t>(self, kOffLobbyMatchType, matchType);
  Put<std::uint16_t>(self, kOffLobbyTeam, team);
  Put<std::uint8_t>(self, kOffLobbyType, lobbyType);
  Put<std::uint32_t>(self, kOffFlags, offline ? (flags | kFlagOfflineLobby) : (flags & ~kFlagOfflineLobby));
}

// CNSISocial::EnterLobby (libpnsovr 0x208478).
void SlotEnterLobby(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team, std::uint8_t lobbyType,
                    std::uint32_t offline) {
  EnterLobbyFields(self, uuid, matchType, team, lobbyType, offline != 0);
}

// CNSISocial::EnterOnlineLobby (0x2084a8) forwards to EnterLobby with offline = 0.
void SlotEnterOnlineLobby(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team,
                          std::uint8_t lobbyType) {
  EnterLobbyFields(self, uuid, matchType, team, lobbyType, false);
}

// CNSISocial::EnterOfflineLobby (0x2084b8) forwards with team = 0xffff and offline = 1.
void SlotEnterOfflineLobby(void* self, const void* uuid, std::uint64_t matchType, std::uint8_t lobbyType) {
  EnterLobbyFields(self, uuid, matchType, UINT16_MAX, lobbyType, true);
}

// CNSISocial::ExitLobby (0x2084d0): the invalid uuid, no match type, no team, the private lobby type, and
// the offline bit cleared. Nothing about the party is touched.
void SlotExitLobby(void* self) {
  ResetLobbyFields(self);
  Put<std::uint32_t>(self, kOffFlags, Get<std::uint32_t>(self, kOffFlags) & ~kFlagOfflineLobby);
}

// ---- friends --------------------------------------------------------------------------------
// CNSOVRSocial keeps a count, an online count and parallel id / name / status arrays with the online
// friends first: FriendStatus(i) is 2 for i < OnlineFriendCount and 0 after it (libpnsovr 0x2061fc).

// The friends tab is open while the game reads the list (nevr_social_party::NoteFriendsViewed); the poll in
// UpdateCollect refreshes it while it is.
void NoteFriendsViewed(Impl& impl) { Party(impl).NoteFriendsViewed(Now(impl)); }

std::uint32_t SlotFriendCount(void* self) {
  NoteFriendsViewed(*OwnerOf(self));
  return Friends(*OwnerOf(self)).Count();
}
std::uint32_t SlotOnlineFriendCount(void* self) { return Friends(*OwnerOf(self)).Online(); }
std::uint32_t SlotOfflineFriendCount(void* self) { return Friends(*OwnerOf(self)).Offline(); }

std::uint64_t SlotFriendId(void* self, std::uint32_t index) {
  NoteFriendsViewed(*OwnerOf(self));
  std::uint64_t id = 0;
  Friends(*OwnerOf(self)).IdAt(index, &id);
  return id;
}

const char* SlotFriendName(void* self, std::uint32_t index) {
  NoteFriendsViewed(*OwnerOf(self));
  return Friends(*OwnerOf(self)).NameAt(index);
}

std::uint32_t SlotFriendStatus(void* self, std::uint32_t index) {
  NoteFriendsViewed(*OwnerOf(self));
  return Friends(*OwnerOf(self)).OnlineAt(index) ? 2U : 0U;
}

const char* SlotFriendStatusString(void* self, std::uint32_t index) {
  return Friends(*OwnerOf(self)).StatusTextAt(index);
}

bool IsMember(const nevr_social_party::View& view, std::uint64_t id) {
  for (const nevr_social_party::Member& member : view.members) {
    if (member.id == id) return true;
  }
  return false;
}

// A friend is invitable only while the local party is joinable, the friend is not already a member and
// is reachable (online here). It is not a function of the friend alone.
std::uint32_t SlotFriendIsInvitable(void* self, std::uint32_t index) {
  Impl& impl = *OwnerOf(self);
  std::uint64_t id = 0;
  if (!Friends(impl).IdAt(index, &id) || !Friends(impl).OnlineAt(index)) return 0;
  const auto view = CurrentView(impl);
  return PartyJoinable(impl, *view) && !IsMember(*view, id) ? 1U : 0U;
}

// OpenSendInviteUI / OpenNewSendInviteUI(LocalUserID): the party tab's "Invite Members" (#318). pnsovr opened
// the Oculus friend picker here and the game passes no target; the NEVR facade has no picker, so no invite is
// sent (inviting every online friend on one tap would send invites nobody asked for). The Friends tab's
// invite works and is the way to invite. OpenPartyUI (slot 43, no target) is called when the tab opens and
// must never invite.
void SlotInviteUINoTarget(void*, std::uint32_t) {
  LogFields(LogLevel::kInfo, "social_invite_ui",
            {{"result", "no_target_no_invite_sent"}, {"hint", "use the Friends tab"}});
}

// OpenNewSendInviteUI(LocalUserID, UserAccountID): the game names the user to invite, so this is the same
// invite SlotSendInvite sends for the Friends tab.
void SlotInviteUITarget(void* self, std::uint32_t, std::uint64_t target) { SlotSendInvite(self, target); }

// OpenPartyUI(LocalUserID, UserAccountID): open the party UI for a user. Nothing shows the game calls it from
// an invite action (it may be "view this user's party"), so it sends nothing and says so. It becomes an invite
// only once a headset logcat shows the game reaches it from one.
void SlotPartyUITarget(void*, std::uint32_t, std::uint64_t target) {
  LogFields(LogLevel::kInfo, "social_party_ui",
            {{"result", "no_invite_sent"}, {"slot", 44}, {"target", static_cast<long long>(target)}});
}

std::uint64_t SlotFriendPartyId(void* self, std::uint32_t index) { return Friends(*OwnerOf(self)).PartyIdAt(index); }

// CNSISocial::FriendIsJoinable (libpnsovr 0x208510) is FriendPartyId(i) != 0.
std::uint32_t SlotFriendIsJoinable(void* self, std::uint32_t index) { return SlotFriendPartyId(self, index) != 0 ? 1U : 0U; }

// ---- recently met ---------------------------------------------------------------------------

std::uint32_t SlotRefreshingRecentlyMet(void* self) {
  Impl& impl = *OwnerOf(self);
  bool timedOut = false;
  const bool busy = Recent(impl).Refreshing(Now(impl), &timedOut);
  if (timedOut) {
    LogFields(LogLevel::kWarn, "social_recently_met",
              {{"result", "refresh_timeout"}, {"seconds", static_cast<long long>(nevr_social_roster::RecentList::kRefreshSeconds)}});
  }
  return busy ? 1U : 0U;
}

void SlotRefreshRecentlyMet(void* self) {
  Impl& impl = *OwnerOf(self);
  if (!Recent(impl).BeginRefresh(Now(impl))) {
    LogFields(LogLevel::kInfo, "social_recently_met", {{"result", "already_in_flight"}});
    return;
  }
  const std::vector<nevr_social_party::Message> request = Party(impl).RefreshRecentlyMet();
  const bool sent = !request.empty() && impl.ports.send != nullptr && impl.ports.send(request);
  if (!sent) {
    Recent(impl).EndRefresh();
    if (!request.empty()) Count(g_sendFailed);
  }
  LogRequests("recently met refresh", request, sent, 0);
  LogFields(sent ? LogLevel::kInfo : LogLevel::kWarn, "social_recently_met", {{"result", sent ? "requested" : "NOT_sent"}});
}

std::uint32_t SlotRecentCount(void* self) { return Recent(*OwnerOf(self)).Count(); }
std::uint32_t SlotRecentOnline(void* self) { return Recent(*OwnerOf(self)).Online(); }
std::uint32_t SlotRecentOffline(void* self) {
  Impl& impl = *OwnerOf(self);
  return Recent(impl).Count() - Recent(impl).Online();
}

std::uint64_t SlotRecentId(void* self, std::uint32_t index) {
  std::uint64_t id = 0;
  Recent(*OwnerOf(self)).IdAt(index, &id);
  return id;
}

const char* SlotRecentName(void* self, std::uint32_t index) { return Recent(*OwnerOf(self)).NameAt(index); }
std::uint32_t SlotRecentStatus(void* self, std::uint32_t index) { return Recent(*OwnerOf(self)).OnlineAt(index) ? 2U : 0U; }
const char* SlotRecentStatusString(void* self, std::uint32_t index) { return Recent(*OwnerOf(self)).TextAt(index); }

std::uint32_t SlotRecentIsInvitable(void* self, std::uint32_t index) {
  Impl& impl = *OwnerOf(self);
  std::uint64_t id = 0;
  if (!Recent(impl).IdAt(index, &id) || !Recent(impl).OnlineAt(index)) return 0;
  const auto view = CurrentView(impl);
  return PartyJoinable(impl, *view) && !IsMember(*view, id) ? 1U : 0U;
}

std::uint64_t SlotRecentPartyId(void* self, std::uint32_t index) { return Recent(*OwnerOf(self)).PartyIdAt(index); }
std::uint32_t SlotRecentIsJoinable(void* self, std::uint32_t index) { return SlotRecentPartyId(self, index) != 0 ? 1U : 0U; }

// ---- invites --------------------------------------------------------------------------------

const nevr_social_party::Invite* InviteAt(const nevr_social_party::View& view, std::uint32_t index) {
  if (index >= view.invites.size()) return nullptr;
  return &view.invites[view.invites.size() - 1 - index];  // newest first
}

std::uint32_t SlotInviteCount(void* self) { return static_cast<std::uint32_t>(CurrentView(*OwnerOf(self))->invites.size()); }

const char* SlotInviteSender(void* self, std::uint32_t index) {
  const auto view = CurrentView(*OwnerOf(self));
  const nevr_social_party::Invite* invite = InviteAt(*view, index);
  return invite != nullptr ? invite->senderName.c_str() : "";
}

std::uint64_t SlotInviteSentTime(void* self, std::uint32_t index) {
  const auto view = CurrentView(*OwnerOf(self));
  const nevr_social_party::Invite* invite = InviteAt(*view, index);
  return invite != nullptr ? invite->sentTime : 0;
}

// ---- the table ------------------------------------------------------------------------------

void ReportFailure(Impl* impl, std::size_t index) {
  if (impl == nullptr) return;
  const std::uint32_t n = impl->slotFailures.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n % 4096 == 0) {
    LogFields(LogLevel::kError, "social_slot_exception",
              {{"slot", static_cast<long long>(index)}, {"name", kSlotNames[index]}, {"failures", n}, {"action", "zero_result"}});
  }
}

void TraceCall(Impl* impl, std::size_t index) {
  if (impl == nullptr) return;
  const std::uint32_t n = impl->slotCalls[index].fetch_add(1, std::memory_order_relaxed) + 1;
  const std::uint64_t now = Now(*impl);
  if (n > kTraceFirstCalls) {
    std::uint64_t last = impl->slotTraceAt[index].load(std::memory_order_relaxed);
    if (now >= last && now - last < kTraceGapSeconds) return;
    if (!impl->slotTraceAt[index].compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
  } else {
    impl->slotTraceAt[index].store(now, std::memory_order_relaxed);
  }
  LogFields(LogLevel::kInfo, "social_slot", {{"slot", static_cast<long long>(index)}, {"name", kSlotNames[index]}, {"call", n}});
}

// Wraps one slot function: no exception leaves it, a null or foreign object is answered with the
// zero value, and the call is counted and (for the first calls) logged.
template <typename First, typename... Rest>
void* SelfOf(First first, Rest...) {
  return first;
}

template <std::size_t Index, auto Fn>
struct Guarded;

template <std::size_t Index, typename R, typename... A, R (*Fn)(A...)>
struct Guarded<Index, Fn> {
  static R Call(A... args) noexcept {
    Impl* impl = OwnerOf(SelfOf(args...));
    if (impl == nullptr) return R();
    TraceCall(impl, Index);
    try {
      return Fn(args...);
    } catch (const std::exception&) {
      ReportFailure(impl, Index);
      return R();
    }
  }
};

template <std::size_t Index, auto Fn>
SlotWord Entry() {
  return reinterpret_cast<SlotWord>(&Guarded<Index, Fn>::Call);
}

void BuildVtable(std::array<SlotWord, kSlotCount>* table) {
  std::array<SlotWord, kSlotCount>& t = *table;
  t[kSwapMembers] = Entry<kSwapMembers, &SlotNothingU32U32>();
  t[kRemoveRemoteMember] = Entry<kRemoveRemoteMember, &SlotNothingU32>();
  t[kJoinInternal] = reinterpret_cast<SlotWord>(&internal::SlotJoinInternalEntry);
  t[kLeaveInternal] = Entry<kLeaveInternal, &SlotNothingPtr>();
  t[kJoinableInternal] = Entry<kJoinableInternal, &SlotJoinableInternal>();
  t[kSetJoinableInternal] = Entry<kSetJoinableInternal, &SlotSetJoinableInternal>();
  t[kShareDataParty] = Entry<kShareDataParty, &SlotNothing>();
  t[kShareDataMember] = Entry<kShareDataMember, &SlotNothingU32>();
  t[kSendInviteInternal] = Entry<kSendInviteInternal, &SlotSendInvite>();
  t[kInitialize] = Entry<kInitialize, &SlotInitialize>();
  t[kShutdown] = Entry<kShutdown, &SlotShutdown>();
  t[kDestructorComplete] = Entry<kDestructorComplete, &SlotDestructor>();
  t[kDestructorDeleting] = Entry<kDestructorDeleting, &SlotDestructor>();
  t[kReset] = reinterpret_cast<SlotWord>(&internal::SlotResetEntry);
  t[kUpdate] = reinterpret_cast<SlotWord>(&internal::SlotUpdateEntry);
  t[kAddMember] = Entry<kAddMember, &SlotAddMember>();
  t[kRemoveMember] = Entry<kRemoveMember, &SlotNothingU32>();
  t[kSetJoinPolicy] = Entry<kSetJoinPolicy, &SlotSetJoinPolicy>();
  t[kLeave] = Entry<kLeave, &SlotLeave>();
  t[kPassOwnership] = Entry<kPassOwnership, &SlotPassOwnership>();
  t[kKick] = Entry<kKick, &SlotKick>();
  t[kReady] = Entry<kReady, &SlotReady>();
  t[kJoinPolicy] = Entry<kJoinPolicy, &SlotJoinPolicy>();
  t[kJoinable] = Entry<kJoinable, &SlotJoinable>();
  t[kHost] = Entry<kHost, &SlotHost>();
  t[kIsHost] = Entry<kIsHost, &SlotIsHost>();
  t[kId] = Entry<kId, &SlotId>();
  t[kMemberCount] = Entry<kMemberCount, &SlotMemberCount>();
  t[kMemberId] = Entry<kMemberId, &SlotMemberId>();
  t[kMemberName] = Entry<kMemberName, &SlotMemberName>();
  t[kLocalId] = Entry<kLocalId, &SlotLocalId>();
  t[kMemberDataWritable] = Entry<kMemberDataWritable, &SlotMemberDataWritable>();
  t[kEnterLobby] = Entry<kEnterLobby, &SlotEnterLobby>();
  t[kEnterOnlineLobby] = Entry<kEnterOnlineLobby, &SlotEnterOnlineLobby>();
  t[kEnterOfflineLobby] = Entry<kEnterOfflineLobby, &SlotEnterOfflineLobby>();
  t[kExitLobby] = Entry<kExitLobby, &SlotExitLobby>();
  t[kEnterGame] = Entry<kEnterGame, &SlotNothingPtr>();
  t[kExitGame] = Entry<kExitGame, &SlotNothing>();
  t[kOpenFriendRequestUI] = Entry<kOpenFriendRequestUI, &SlotOpenFriendRequestUI>();
  t[kOpenSendInviteUI] = Entry<kOpenSendInviteUI, &SlotInviteUINoTarget>();
  t[kOpenNewSendInviteUI] = Entry<kOpenNewSendInviteUI, &SlotInviteUINoTarget>();
  t[kOpenNewSendInviteUITarget] = Entry<kOpenNewSendInviteUITarget, &SlotInviteUITarget>();
  t[kOpenRecvInviteUI] = Entry<kOpenRecvInviteUI, &SlotNothingU32>();
  t[kOpenPartyUI] = Entry<kOpenPartyUI, &SlotNothingU32>();
  t[kOpenPartyUITarget] = Entry<kOpenPartyUITarget, &SlotPartyUITarget>();
  t[kRefreshingFriends] = Entry<kRefreshingFriends, &SlotZero32>();
  t[kRefreshFriends] = Entry<kRefreshFriends, &SlotRefreshFriends>();
  t[kFriendCount] = Entry<kFriendCount, &SlotFriendCount>();
  t[kOnlineFriendCount] = Entry<kOnlineFriendCount, &SlotOnlineFriendCount>();
  t[kOfflineFriendCount] = Entry<kOfflineFriendCount, &SlotOfflineFriendCount>();
  t[kFriendId] = Entry<kFriendId, &SlotFriendId>();
  t[kFriendName] = Entry<kFriendName, &SlotFriendName>();
  t[kFriendStatus] = Entry<kFriendStatus, &SlotFriendStatus>();
  t[kFriendStatusString] = Entry<kFriendStatusString, &SlotFriendStatusString>();
  t[kFriendIsInvitable] = Entry<kFriendIsInvitable, &SlotFriendIsInvitable>();
  t[kFriendIsJoinable] = Entry<kFriendIsJoinable, &SlotFriendIsJoinable>();
  t[kFriendPartyId] = Entry<kFriendPartyId, &SlotFriendPartyId>();
  t[kRefreshingRecentlyMetUsers] = Entry<kRefreshingRecentlyMetUsers, &SlotRefreshingRecentlyMet>();
  t[kRefreshRecentlyMetUsers] = Entry<kRefreshRecentlyMetUsers, &SlotRefreshRecentlyMet>();
  t[kRecentlyMetUserCount] = Entry<kRecentlyMetUserCount, &SlotRecentCount>();
  t[kOnlineRecentlyMetUserCount] = Entry<kOnlineRecentlyMetUserCount, &SlotRecentOnline>();
  t[kOfflineRecentlyMetUserCount] = Entry<kOfflineRecentlyMetUserCount, &SlotRecentOffline>();
  t[kRecentlyMetUserId] = Entry<kRecentlyMetUserId, &SlotRecentId>();
  t[kRecentlyMetUserName] = Entry<kRecentlyMetUserName, &SlotRecentName>();
  t[kRecentlyMetUserStatus] = Entry<kRecentlyMetUserStatus, &SlotRecentStatus>();
  t[kRecentlyMetUserStatusString] = Entry<kRecentlyMetUserStatusString, &SlotRecentStatusString>();
  t[kRecentlyMetUserIsInvitable] = Entry<kRecentlyMetUserIsInvitable, &SlotRecentIsInvitable>();
  t[kRecentlyMetUserIsJoinable] = Entry<kRecentlyMetUserIsJoinable, &SlotRecentIsJoinable>();
  t[kRecentlyMetUserPartyId] = Entry<kRecentlyMetUserPartyId, &SlotRecentPartyId>();
  t[kRefreshingInvites] = Entry<kRefreshingInvites, &SlotZero32>();
  t[kRefreshInvites] = Entry<kRefreshInvites, &SlotNothing>();
  t[kInviteCount] = Entry<kInviteCount, &SlotInviteCount>();
  t[kInviteSender] = Entry<kInviteSender, &SlotInviteSender>();
  t[kInviteSentTime] = Entry<kInviteSentTime, &SlotInviteSentTime>();
  t[kAcceptInvite] = reinterpret_cast<SlotWord>(&internal::SlotAcceptInviteEntry);
  t[kDismissInvite] = Entry<kDismissInvite, &SlotDismissInvite>();
}

}  // namespace

// ---- the seam to social_game_calls.cpp (social_internal.h) ----------------------------------------
// Each function contains its own failures and returns before the game is called.

namespace internal {

void TraceSlotCall(void* self, std::size_t slot) noexcept { TraceCall(OwnerOf(self), slot); }

void NoteCallbackDelivered(void* self, std::size_t callback) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl != nullptr) impl->callbackCalls.fetch_add(1, std::memory_order_relaxed);
  switch (callback) {
    case kCbCreated: Count(g_cbCreated); break;
    case kCbMemberJoined: Count(g_cbMemberJoined); break;
    case kCbJoinFailed: Count(g_cbJoinFailed); break;
    default: Count(g_cbOther); break;
  }
}

std::uint64_t UpdatePrepare(void* self, const void* params) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return 0;
  try {
    // Requests the server never answered are given up before the create / join decision, so the same
    // Update asks again (the create) or lets the deferred join through.
    ExpirePending(*impl);
    const std::uint64_t deferredJoin = Party(*impl).DeferredJoin();
    if (deferredJoin != 0) return deferredJoin;
    MaybeCreateParty(*impl, params);
  } catch (const std::exception&) {
    ReportFailure(impl, kUpdate);
  }
  return 0;
}

namespace {

EventKind KindOf(nevr_social_party::EventKind kind, bool* deliver) {
  using Kind = nevr_social_party::EventKind;
  *deliver = true;
  switch (kind) {
    case Kind::kCreated: return kEvCreated;
    case Kind::kJoined: return kEvJoined;
    case Kind::kJoinFailed: return kEvJoinFailed;
    case Kind::kUpdated: return kEvUpdated;
    case Kind::kHostChanged: return kEvHostChanged;
    case Kind::kLeft: return kEvLeft;
    case Kind::kKicked: return kEvKicked;
    case Kind::kMemberJoined: return kEvMemberJoined;
    case Kind::kMemberUpdated: return kEvMemberUpdated;
    case Kind::kMemberLeft: return kEvMemberLeft;
    case Kind::kInviteReceived: return kEvInviteReceived;
    case Kind::kInviteFailed: break;  // no game callback is driven from here
  }
  *deliver = false;
  return kEvUpdated;
}

}  // namespace

namespace {

bool Coalescible(nevr_social_party::EventKind kind) {
  using Kind = nevr_social_party::EventKind;
  return kind == Kind::kUpdated || kind == Kind::kMemberUpdated || kind == Kind::kInviteReceived;
}

// Adds one event to the carry queue. A repeat of a callback that carries nothing a second one adds is merged
// into the one already due: any pending InviteReceived (its argument is always 0 and the invite list it
// announces is read when it runs), or an Updated / MemberUpdated of the same index directly after its twin.
// Past kSoftQueueLimit the oldest coalescible event makes room; if there is none the queue grows to
// kHardQueueLimit, and only past that is the newest event dropped. Everything dropped is counted.
void Enqueue(Impl& impl, nevr_social_party::Event&& event, EventBatch* out) {
  using Kind = nevr_social_party::EventKind;
  std::deque<nevr_social_party::Event>& queue = impl.queuedEvents;
  if (event.kind == Kind::kInviteReceived) {
    for (const nevr_social_party::Event& queued : queue) {
      if (queued.kind == Kind::kInviteReceived) return;
    }
  } else if (Coalescible(event.kind) && !queue.empty() && queue.back().kind == event.kind &&
             queue.back().index == event.index) {
    return;
  }
  if (queue.size() >= kSoftQueueLimit) {
    bool made = false;
    for (auto it = queue.begin(); it != queue.end(); ++it) {
      if (!Coalescible(it->kind)) continue;
      queue.erase(it);
      Count(g_eventsDropped);
      ++out->dropped;
      made = true;
      break;
    }
    if (!made && queue.size() >= kHardQueueLimit) {
      Count(g_eventsDropped);
      ++out->dropped;
      return;
    }
  }
  queue.push_back(std::move(event));
}

int PositionOf(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == id) return static_cast<int>(i);
  }
  return -1;
}

// Moves the events the model queued into the carry queue, showing the game only the members it can hold.
// `ids` is every member's id in model order after this frame's publish; the game sees the first
// kMemberJsonSlots. A member past that is hidden: its MemberJoined, MemberUpdated and MemberLeft never reach the
// game (any index at or past the array would be read out of bounds, PartyMemberJoinedCB 0x126f8ac), and when
// a visible member leaves, the first hidden one moves into the window and is announced then. A member
// callback's index is the member's position now, not the one it had when the event was queued.
void QueueEvents(Impl& impl, std::vector<nevr_social_party::Event>& events, const std::vector<std::uint64_t>& ids,
                 std::uint64_t partyId, EventBatch* out) {
  using Kind = nevr_social_party::EventKind;
  for (nevr_social_party::Event& event : events) {
    switch (event.kind) {
      case Kind::kMemberJoined: {
        const int at = PositionOf(ids, event.id);
        if (at >= 0 && static_cast<std::size_t>(at) < kMemberJsonSlots) {
          event.index = static_cast<std::uint32_t>(at);
          impl.hiddenMembers.erase(event.id);
          Enqueue(impl, std::move(event), out);
        } else {
          impl.hiddenMembers.insert(event.id);
          Count(g_membersHidden);
          if (impl.hiddenLoggedParty != partyId) {
            impl.hiddenLoggedParty = partyId;
            LogFields(LogLevel::kWarn, "social_members_hidden",
                      {{"party", static_cast<long long>(partyId)}, {"capacity", static_cast<long long>(kMemberJsonSlots)},
                       {"note", "members past the game's array stay in the model and are not shown to the game"}});
          }
        }
        break;
      }
      case Kind::kMemberUpdated: {
        const int at = PositionOf(ids, event.id);
        if (at >= 0 && static_cast<std::size_t>(at) < kMemberJsonSlots) {
          event.index = static_cast<std::uint32_t>(at);
          Enqueue(impl, std::move(event), out);
        }
        break;
      }
      case Kind::kMemberLeft:
        if (impl.hiddenMembers.erase(event.id) == 0) Enqueue(impl, std::move(event), out);
        break;
      case Kind::kCreated:
      case Kind::kJoined:
      case Kind::kLeft:
      case Kind::kKicked:
        impl.hiddenMembers.clear();  // a different party, or none
        Enqueue(impl, std::move(event), out);
        break;
      default:
        Enqueue(impl, std::move(event), out);
        break;
    }
  }
  // A hidden member now inside the window (a visible one left): the game has not heard of it yet.
  const std::size_t window = ids.size() < kMemberJsonSlots ? ids.size() : kMemberJsonSlots;
  for (std::size_t i = 0; i < window; ++i) {
    if (impl.hiddenMembers.erase(ids[i]) != 0) {
      Enqueue(impl, nevr_social_party::MakeEvent(Kind::kMemberJoined, static_cast<std::uint32_t>(i), ids[i]), out);
    }
  }
}

}  // namespace

void UpdateCollect(void* self, EventBatch* out, JsonPlan* json) noexcept {
  if (out == nullptr || json == nullptr) return;
  out->count = 0;
  out->dropped = 0;
  json->count = 0;
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return;
  try {
    // The events are drained before the view is published, so the view is at least as new as every event: a
    // member an event names is in it unless it has left since (see QueueEvents).
    // The friends tab stays fresh while it is open (#57): nothing is sent for a closed tab.
    const std::vector<nevr_social_party::Message> poll = Party(*impl).PollFriendsWhileOpen(Now(*impl));
    if (!poll.empty()) SendParty(*impl, "friends poll", poll);
    std::vector<nevr_social_party::Event> events = Party(*impl).DrainEvents();
    const std::vector<std::uint64_t> ids = PublishView(*impl);
    const auto view = CurrentView(*impl);
    SyncObject(*impl, *view);
    PlanReceivedData(*impl, *view, json);
    // Everything the model queued joins the carry queue, oldest first; one batch is delivered now and the
    // rest next frame, in order (see Enqueue for what is merged and what may be dropped).
    QueueEvents(*impl, events, ids, ViewRoomId(*view), out);
    if (out->dropped != 0 && !impl->queueOverflowLogged) {
      impl->queueOverflowLogged = true;
      LogFields(LogLevel::kError, "social_events_dropped",
                {{"soft_limit", static_cast<long long>(kSoftQueueLimit)}, {"hard_limit", static_cast<long long>(kHardQueueLimit)},
                 {"note", "coalescible events dropped for room, or the newest past the hard limit; counted by social_events_dropped"}});
    }
    while (!impl->queuedEvents.empty() && out->count < kMaxPendingEvents) {
      const nevr_social_party::Event& event = impl->queuedEvents.front();
      bool deliver = false;
      const EventKind kind = KindOf(event.kind, &deliver);
      if (deliver) {
        PendingEvent& slot = out->events[out->count++];
        slot.kind = kind;
        slot.index = event.index;
        slot.code = event.code;
        slot.id = event.id;
        std::strncpy(slot.name, event.name.c_str(), kEventNameBytes - 1);
        slot.name[kEventNameBytes - 1] = '\0';
      }
      impl->queuedEvents.pop_front();
    }
  } catch (const std::exception&) {
    ReportFailure(impl, kUpdate);
  }
}

CJsonResetFn ResetPrepare(void* self) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return nullptr;
  try {
    SendParty(*impl, "party reset", Party(*impl).ResetParty());
    const CJsonResetFn reset = g_cjsonReset.load(std::memory_order_acquire);
    ResetBase(*impl, reset != nullptr);
    impl->createSince = 0;
    impl->joinSince = 0;
    impl->lockSince = 0;
    // What was loaded or shared belonged to the party that is gone.
    impl->slotMember.fill(0);
    for (nevr_social_party::JsonText& data : impl->slotData) data.reset();
    impl->partyDataLoaded.reset();
    impl->sharedParty = 0;
    impl->memberDataWritten.store(false, std::memory_order_relaxed);
    if (reset == nullptr) {
      Count(g_jsonFailed);
      LogFields(LogLevel::kWarn, "social_reset", {{"cjson_reset", "unavailable"}, {"action", "party_cjson_left_alone"}});
    }
    return reset;
  } catch (const std::exception&) {
    ReportFailure(impl, kReset);
  }
  return nullptr;
}

void JsonApplied(void* self, const JsonPlan* json) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr || json == nullptr) return;
  try {
    for (std::uint32_t i = 0; i < json->count; ++i) {
      const JsonOp& op = json->ops[i];
      if (op.ok != 0 && op.kind == kJsonLoad) {
        LogFields(LogLevel::kInfo, "social_party_data",
                  {{"result", "loaded"}, {"what", op.slot == kJsonParty ? "party" : "member"},
                   {"slot", op.slot == kJsonParty ? -1LL : static_cast<long long>(op.slot)},
                   {"bytes", static_cast<long long>(op.length)}});
      } else if (op.ok == 0) {
        Count(g_jsonFailed);
        LogFields(LogLevel::kWarn, "social_party_data",
                  {{"result", op.kind == kJsonLoad ? "load_failed" : "clear_failed"},
                   {"what", op.slot == kJsonParty ? "party" : "member"},
                   {"slot", op.slot == kJsonParty ? -1LL : static_cast<long long>(op.slot)},
                   {"bytes", static_cast<long long>(op.length)}});
      }
    }
  } catch (const std::exception&) {
    ReportFailure(impl, kUpdate);
  }
}

// The game's own Update shares what the game wrote: the leader's party data when the game marked it written (flag bit 0,
// which is then cleared), the local member's after MemberDataWritable handed it out; both once more on entering a
// party, so the server holds them from the start (pnsovr 0x1800ac240, slots 7 and 6, on PC the same).
void ShareBegin(void* self, ShareJob* job) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr || job == nullptr) return;
  job->party = 0;
  job->member = 0;
  job->partyOk = 0;
  job->memberOk = 0;
  job->partySize = 0;
  job->memberSize = 0;
  job->capacity = kShareBufferBytes - 1;
  job->partyBuffer = impl->partyBuffer.data();
  job->memberBuffer = impl->memberBuffer.data();
  job->game = CurrentGameJson();
  try {
    const auto view = CurrentView(*impl);
    if (job->game.encode == nullptr || view->partyId == 0 || view->joining) return;
    const bool newParty = view->partyId != impl->sharedParty;
    if (newParty) {
      impl->sharedParty = view->partyId;
      impl->memberDataWritten.store(true, std::memory_order_relaxed);
    }
    const std::uint32_t flags = Get<std::uint32_t>(self, kOffFlags);
    if (view->ownerId == view->selfId && ((flags & kFlagDataWritten) != 0 || newParty)) {
      Put<std::uint32_t>(self, kOffFlags, flags & ~kFlagDataWritten);
      job->party = 1;
    }
    if (impl->memberDataWritten.exchange(false, std::memory_order_relaxed)) job->member = 1;
  } catch (const std::exception&) {
    ReportFailure(impl, kUpdate);
  }
}

namespace {

// One read-out text to the server: the game's empty document is "{}", and only a JSON object is sent.
void ShareOne(Impl& impl, const char* what, std::uint64_t scope, bool ok, const char* buffer, std::uint64_t size) {
  if (!ok) {
    Count(g_jsonFailed);
    LogFields(LogLevel::kWarn, "social_party_data", {{"result", "share_failed"}, {"what", what}, {"note", "the game's CJson could not be read out"}});
    return;
  }
  std::string text(buffer, static_cast<std::size_t>(size));
  while (!text.empty() && text.back() == '\0') text.pop_back();
  if (text.find_first_not_of(" \t\r\n") == std::string::npos) text = "{}";
  const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    Count(g_jsonFailed);
    LogFields(LogLevel::kWarn, "social_party_data", {{"result", "share_rejected"}, {"what", what}, {"bytes", static_cast<long long>(text.size())},
                                                      {"note", "the game's JSON is not an object"}});
    return;
  }
  SendParty(impl, what, Party(impl).ShareData(scope, text));
}

}  // namespace

void ShareFinish(void* self, const ShareJob* job) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr || job == nullptr) return;
  try {
    if (job->party != 0) {
      ShareOne(*impl, "party data (party)", nevr_social_party::kPartyDataScopeParty, job->partyOk != 0, job->partyBuffer, job->partySize);
    }
    if (job->member != 0) {
      ShareOne(*impl, "party data (member)", nevr_social_party::kPartyDataScopeMember, job->memberOk != 0, job->memberBuffer, job->memberSize);
    }
  } catch (const std::exception&) {
    ReportFailure(impl, kUpdate);
  }
}

void UpdateFinish(void* self) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return;
  try {
    SyncHostJoinable(*impl);
  } catch (const std::exception&) {
    ReportFailure(impl, kUpdate);
  }
}

JoinStep JoinBegin(void* self, std::uint64_t partyId) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return JoinStep::kDeferred;
  try {
    nevr_social_party::State& party = Party(*impl);
    if (party.BeginJoin(partyId)) {
      impl->deferredLoggedParty = 0;
      return JoinStep::kAskGate;
    }
    // The model retries a deferred join every Update until the create or join in flight answers: count each
    // deferral, log it once per party (a state change), not once per frame.
    Count(g_joinDeferred);
    if (impl->deferredLoggedParty != partyId) {
      impl->deferredLoggedParty = partyId;
      LogFields(LogLevel::kInfo, "social_join",
                {{"party", static_cast<long long>(partyId)}, {"result", "deferred"},
                 {"deferred_party", static_cast<long long>(party.DeferredJoin())}});
    }
  } catch (const std::exception&) {
    ReportFailure(impl, kJoinInternal);
  }
  return JoinStep::kDeferred;
}

void JoinFinish(void* self, std::uint64_t partyId, bool allowed) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return;
  try {
    nevr_social_party::State& party = Party(*impl);
    if (!allowed) {
      party.AbandonJoin(partyId);
      LogFields(LogLevel::kInfo, "social_join", {{"party", static_cast<long long>(partyId)}, {"result", "declined"}});
      return;
    }
    if (SendParty(*impl, "party join", party.Join(partyId))) {
      if (party.Snapshot().joining) impl->joinSince = Now(*impl);
    } else {
      party.AbandonJoining();  // restores the invites and tells the game the join failed
    }
    impl->deferredLoggedParty = 0;
  } catch (const std::exception&) {
    ReportFailure(impl, kJoinInternal);
  }
}

std::uint64_t InvitePartyAt(void* self, std::uint32_t index) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return 0;
  try {
    const std::uint64_t partyId = Party(*impl).InvitePartyAt(index);
    LogFields(LogLevel::kInfo, "social_accept", {{"index", index}, {"party", static_cast<long long>(partyId)}});
    return partyId;
  } catch (const std::exception&) {
    ReportFailure(impl, kAcceptInvite);
  }
  return 0;
}

}  // namespace internal

// ---- public ---------------------------------------------------------------------------------

Facade::Facade(const Ports& ports) : impl_(std::make_unique<Impl>()) {
  impl_->ports = ports;
  BuildVtable(&impl_->vtable);
  void* object = impl_->object.data();
  const SlotWord* table = impl_->vtable.data();
  Put<const SlotWord*>(object, 0, table);
  Put<Impl*>(object, kOffOwner, impl_.get());
  ResetBase(*impl_, false);
  Put<std::uint32_t>(object, kOffJoinPolicy, nevr_social_party::kJoinPolicyEveryone);
  Put<std::uintptr_t>(object, kOffMemberJson, reinterpret_cast<std::uintptr_t>(impl_->memberJson.data()));
}

Facade::~Facade() {
  if (impl_ != nullptr && impl_->processWide) g_destroyed.fetch_add(1, std::memory_order_relaxed);
}

void* Facade::Object() noexcept { return impl_->object.data(); }

std::uint32_t Facade::InitializeCalls() const noexcept { return impl_->initializeCalls.load(std::memory_order_relaxed); }
std::uint32_t Facade::ShutdownCalls() const noexcept { return impl_->shutdownCalls.load(std::memory_order_relaxed); }
std::uint32_t Facade::SlotCalls(std::size_t slot) const noexcept {
  return slot < kSlotCount ? impl_->slotCalls[slot].load(std::memory_order_relaxed) : 0;
}
std::uint32_t Facade::SlotFailures() const noexcept { return impl_->slotFailures.load(std::memory_order_relaxed); }
std::uint32_t Facade::CallbackCalls() const noexcept { return impl_->callbackCalls.load(std::memory_order_relaxed); }

const Ports& ProductionPorts() {
  static const Ports ports = [] {
    Ports p;
    p.party = &nevr_social_party::Global();
    p.friends = &nevr_social_roster::Global();
    p.recent = &nevr_social_roster::RecentlyMet();
    p.send = &nevr_social_party::Send;
    return p;
  }();
  return ports;
}

void SetPnsovrBias(std::uintptr_t loadBias) noexcept { g_pnsovrBias.store(loadBias, std::memory_order_release); }

void SetGameJson(const GameJson& json) noexcept {
  g_cjsonReset.store(json.reset, std::memory_order_release);
  g_cjsonDecode.store(json.decode, std::memory_order_release);
  g_cjsonEncode.store(json.encode, std::memory_order_release);
}

void NoteFrameIgnored() noexcept { Count(g_framesIgnored); }

FacadeCounters FacadeCountersView() noexcept {
  return FacadeCounters{g_membersHidden, g_eventsDropped, g_sendFailed, g_joinDeferred, g_requestTimeout,
                        g_cbCreated, g_cbMemberJoined, g_cbJoinFailed, g_cbOther,
                        g_jsonFailed, g_framesIgnored};
}

void ResetFacadeCountersForTest() noexcept {
  g_membersHidden.store(0, std::memory_order_relaxed);
  g_eventsDropped.store(0, std::memory_order_relaxed);
  g_sendFailed.store(0, std::memory_order_relaxed);
  g_joinDeferred.store(0, std::memory_order_relaxed);
  g_requestTimeout.store(0, std::memory_order_relaxed);
  g_cbCreated.store(0, std::memory_order_relaxed);
  g_cbMemberJoined.store(0, std::memory_order_relaxed);
  g_cbJoinFailed.store(0, std::memory_order_relaxed);
  g_cbOther.store(0, std::memory_order_relaxed);
  g_jsonFailed.store(0, std::memory_order_relaxed);
  g_framesIgnored.store(0, std::memory_order_relaxed);
}

void SetLocalAccount(std::uint64_t accountId, const char* displayName) {
  nevr_social_party::Global().SetSelf(accountId, displayName != nullptr ? std::string(displayName) : std::string());
  LogFields(LogLevel::kInfo, "social_local_account", {{"account", static_cast<long long>(accountId)}});
}

Facade& Facade::Instance() {
  // Never destroyed, on purpose. The object is handed to the game and read by network threads for as
  // long as the process runs; a function-local static would be destroyed during exit while those
  // threads are still running. The leak is one object at process end, reclaimed by the OS.
  static Facade* const facade = [] {
    Facade* created = new Facade(ProductionPorts());
    created->impl_->processWide = true;
    return created;
  }();
  return *facade;
}

std::uint32_t Facade::ProcessWideDestroyedCountForTest() noexcept { return g_destroyed.load(std::memory_order_relaxed); }

}  // namespace quest_social
