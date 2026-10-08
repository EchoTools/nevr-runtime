#include "quest/social/social_facade.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <string>

#include "hook_log.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_internal.h"

namespace quest_social {
namespace {

using SlotWord = std::uintptr_t;
using sentinel::LogFields;
using sentinel::LogLevel;

constexpr std::size_t kViewRing = 128;  // a name pointer the game reads stays valid this many Updates
constexpr std::uint32_t kTraceFirstCalls = 8;
constexpr std::uint32_t kTraceEvery = 600;
constexpr std::uint64_t kCreateRetrySeconds = 4;
constexpr std::uint8_t kUpdateWantsParty = 1;  // Update's flags byte, bit 0: a party should exist

}  // namespace

static std::atomic<std::uint32_t> g_destroyed{0};

struct Facade::Impl {
  Ports ports;
  alignas(16) std::array<std::uint8_t, kObjectSize> object{};
  std::array<SlotWord, kSlotCount> vtable{};
  alignas(16) std::array<std::uint8_t, 16 * kMemberJsonSlots> memberJson{};  // zeroed CJson, the empty document

  std::mutex viewMutex;
  std::shared_ptr<const SocialParty::View> view = std::make_shared<const SocialParty::View>();
  std::array<std::shared_ptr<const SocialParty::View>, kViewRing> retired{};
  std::size_t retiredNext = 0;

  std::array<std::atomic<std::uint32_t>, kSlotCount> slotCalls{};
  std::atomic<std::uint32_t> initializeCalls{0};
  std::atomic<std::uint32_t> shutdownCalls{0};
  std::atomic<std::uint32_t> slotFailures{0};
  std::atomic<std::uint32_t> callbackCalls{0};

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

SocialParty::State& Party(Impl& impl) { return *impl.ports.party; }
SocialRoster::Roster& Friends(Impl& impl) { return *impl.ports.friends; }
SocialRoster::RecentList& Recent(Impl& impl) { return *impl.ports.recent; }

std::shared_ptr<const SocialParty::View> CurrentView(Impl& impl) {
  std::lock_guard<std::mutex> guard(impl.viewMutex);
  return impl.view;
}

void PublishView(Impl& impl) {
  SocialParty::View next = Party(impl).Snapshot();
  // The game's party UI reads member 0, the local user, whether or not a party exists, so the local
  // user is the only member until a party replaces the list (CNSOVRSocial counts the local member too).
  if (next.members.empty() && next.selfId != 0) {
    SocialParty::Member self;
    self.id = next.selfId;
    self.name = next.selfName.empty() ? std::to_string(next.selfId) : next.selfName;
    next.members.push_back(self);
  }
  auto shared = std::make_shared<const SocialParty::View>(std::move(next));
  std::lock_guard<std::mutex> guard(impl.viewMutex);
  impl.retired[impl.retiredNext] = impl.view;
  impl.retiredNext = (impl.retiredNext + 1) % impl.retired.size();
  impl.view = std::move(shared);
}

std::uint64_t ViewRoomId(const SocialParty::View& view) {
  return view.partyId != 0 ? view.partyId : view.joiningPartyId;
}

// ---- sending --------------------------------------------------------------------------------

void SendParty(Impl& impl, const char* what, const std::vector<SocialParty::Message>& messages) {
  if (messages.empty()) {
    LogFields(LogLevel::kInfo, "social_send", {{"what", what}, {"count", 0}, {"sent", "nothing_to_send"}});
    return;
  }
  const bool sent = impl.ports.send != nullptr && impl.ports.send(messages);
  LogFields(sent ? LogLevel::kInfo : LogLevel::kWarn, "social_send",
            {{"what", what}, {"count", static_cast<long long>(messages.size())}, {"sent", sent ? "yes" : "NOT_sent"}});
}

// ---- object fields the game reads directly --------------------------------------------------

void ResetLobbyFields(void* self) {
  Put<std::uint64_t>(self, kOffLobbyMatchType, UINT64_MAX);
  Put<std::uint16_t>(self, kOffLobbyTeam, UINT16_MAX);
  Put<std::uint8_t>(self, kOffLobbyType, 2);
}

void SyncObject(Impl& impl, const SocialParty::View& view) {
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
bool PartyJoinable(const Impl& impl, const SocialParty::View& view) {
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
void SlotNothingU32U64(void*, std::uint32_t, std::uint64_t) {}
void SlotNothingPtr(void*, const void*) {}
std::uint32_t SlotZero32(void*) { return 0; }

void SlotSendInvite(void* self, std::uint64_t target) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, "party invite", Party(impl).SendInvite(target));
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
  SendParty(impl, "refresh friends", Party(impl).RefreshFriends());
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

std::uint32_t SlotMemberCount(void* self) {
  return static_cast<std::uint32_t>(CurrentView(*OwnerOf(self))->members.size());
}

std::uint64_t SlotMemberId(void* self, std::uint32_t index) {
  const auto view = CurrentView(*OwnerOf(self));
  return index < view->members.size() ? view->members[index].id : 0;
}

const char* SlotMemberName(void* self, std::uint32_t index) {
  const auto view = CurrentView(*OwnerOf(self));
  return index < view->members.size() ? view->members[index].name.c_str() : "";
}

// CNSOVRSocial::LocalId (libpnsovr 0x20527c): member 0 is local user 0, anyone else is no local user.
std::uint64_t SlotLocalId(void*, std::uint32_t index) { return index == 0 ? 0 : UINT64_MAX; }

// MemberDataWritable hands out the local member's CJson. Party data (the headset type and other
// per-member JSON) needs the game's CJson functions and is not carried yet; null is what CNSOVRSocial
// answers for every case but the local member (libpnsovr 0x205308), so the game already handles it.
std::uint64_t SlotMemberDataWritable(void*, std::uint32_t) { return 0; }

std::uint32_t SlotJoinableInternal(void* self) { return CurrentView(*OwnerOf(self))->locked ? 0U : 1U; }

void SlotSetJoinableInternal(void* self, std::uint32_t joinable) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, joinable != 0 ? "party unlock" : "party lock", Party(impl).SetLocked(joinable == 0));
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
void ResetBase(Impl& impl) {
  void* self = impl.object.data();
  std::memset(Bytes(self) + kOffPartyJson, 0, 16);
  std::memset(impl.memberJson.data(), 0, impl.memberJson.size());
  Put<std::uint32_t>(self, kOffFlags, (Get<std::uint32_t>(self, kOffFlags) & ~kFlagDataWritten) | kFlagJoinable);
  Put<std::uint32_t>(self, kOffLocalCount, 0);
  Put<std::uint32_t>(self, kOffMemberCount, 0);
  ResetLobbyFields(self);
}

// Reset leaves the party the user is in (no Left callback), then clears the object. The game follows every
// call with AddMember.
void SlotReset(void* self) {
  Impl& impl = *OwnerOf(self);
  SendParty(impl, "party reset", Party(impl).ResetParty());
  ResetBase(impl);
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
  const std::uint64_t now = Now(impl);
  if (impl.createTried && now - impl.lastCreate < kCreateRetrySeconds) return;
  const std::vector<SocialParty::Message> request = Party(impl).CreateParty();
  if (request.empty()) return;
  impl.createTried = true;
  impl.lastCreate = now;
  SendParty(impl, "party create", request);
}

void SyncHostJoinable(Impl& impl) {
  const auto view = CurrentView(impl);
  if (view->partyId == 0 || view->joining || view->ownerId != view->selfId) return;
  const std::uint32_t wanted = HostWantsJoinable(impl) ? 1U : 0U;
  const std::uint32_t current = view->locked ? 0U : 1U;
  if (wanted != current) SendParty(impl, wanted != 0 ? "party unlock" : "party lock", Party(impl).SetLocked(wanted == 0));
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
  Impl& impl = *OwnerOf(self);
  std::uint8_t invalid[16] = {};
  if (impl.ports.invalidUuid != nullptr) std::memcpy(invalid, impl.ports.invalidUuid, sizeof(invalid));
  std::memcpy(Bytes(self) + kOffLobbyUuid, invalid, sizeof(invalid));
  ResetLobbyFields(self);
  Put<std::uint32_t>(self, kOffFlags, Get<std::uint32_t>(self, kOffFlags) & ~kFlagOfflineLobby);
}

// ---- friends --------------------------------------------------------------------------------
// CNSOVRSocial keeps a count, an online count and parallel id / name / status arrays with the online
// friends first: FriendStatus(i) is 2 for i < OnlineFriendCount and 0 after it (libpnsovr 0x2061fc).

std::uint32_t SlotFriendCount(void* self) { return Friends(*OwnerOf(self)).Count(); }
std::uint32_t SlotOnlineFriendCount(void* self) { return Friends(*OwnerOf(self)).Online(); }
std::uint32_t SlotOfflineFriendCount(void* self) { return Friends(*OwnerOf(self)).Offline(); }

std::uint64_t SlotFriendId(void* self, std::uint32_t index) {
  std::uint64_t id = 0;
  Friends(*OwnerOf(self)).IdAt(index, &id);
  return id;
}

const char* SlotFriendName(void* self, std::uint32_t index) { return Friends(*OwnerOf(self)).NameAt(index); }

std::uint32_t SlotFriendStatus(void* self, std::uint32_t index) {
  return Friends(*OwnerOf(self)).OnlineAt(index) ? 2U : 0U;
}

const char* SlotFriendStatusString(void* self, std::uint32_t index) {
  return Friends(*OwnerOf(self)).StatusTextAt(index);
}

bool IsMember(const SocialParty::View& view, std::uint64_t id) {
  for (const SocialParty::Member& member : view.members) {
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
              {{"result", "refresh_timeout"}, {"seconds", static_cast<long long>(SocialRoster::RecentList::kRefreshSeconds)}});
  }
  return busy ? 1U : 0U;
}

void SlotRefreshRecentlyMet(void* self) {
  Impl& impl = *OwnerOf(self);
  if (!Recent(impl).BeginRefresh(Now(impl))) {
    LogFields(LogLevel::kInfo, "social_recently_met", {{"result", "already_in_flight"}});
    return;
  }
  const std::vector<SocialParty::Message> request = Party(impl).RefreshRecentlyMet();
  const bool sent = !request.empty() && impl.ports.send != nullptr && impl.ports.send(request);
  if (!sent) Recent(impl).EndRefresh();
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

const SocialParty::Invite* InviteAt(const SocialParty::View& view, std::uint32_t index) {
  if (index >= view.invites.size()) return nullptr;
  return &view.invites[view.invites.size() - 1 - index];  // newest first
}

std::uint32_t SlotInviteCount(void* self) { return static_cast<std::uint32_t>(CurrentView(*OwnerOf(self))->invites.size()); }

const char* SlotInviteSender(void* self, std::uint32_t index) {
  const auto view = CurrentView(*OwnerOf(self));
  const SocialParty::Invite* invite = InviteAt(*view, index);
  return invite != nullptr ? invite->senderName.c_str() : "";
}

std::uint64_t SlotInviteSentTime(void* self, std::uint32_t index) {
  const auto view = CurrentView(*OwnerOf(self));
  const SocialParty::Invite* invite = InviteAt(*view, index);
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
  if (n <= kTraceFirstCalls || n % kTraceEvery == 0) {
    LogFields(LogLevel::kInfo, "social_slot", {{"slot", static_cast<long long>(index)}, {"name", kSlotNames[index]}, {"call", n}});
  }
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
  t[kReset] = Entry<kReset, &SlotReset>();
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
  t[kOpenSendInviteUI] = Entry<kOpenSendInviteUI, &SlotNothingU32>();
  t[kOpenNewSendInviteUI] = Entry<kOpenNewSendInviteUI, &SlotNothingU32>();
  t[kOpenNewSendInviteUITarget] = Entry<kOpenNewSendInviteUITarget, &SlotNothingU32U64>();
  t[kOpenRecvInviteUI] = Entry<kOpenRecvInviteUI, &SlotNothingU32>();
  t[kOpenPartyUI] = Entry<kOpenPartyUI, &SlotNothingU32>();
  t[kOpenPartyUITarget] = Entry<kOpenPartyUITarget, &SlotNothingU32U64>();
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

void NoteCallbackDelivered(void* self) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl != nullptr) impl->callbackCalls.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t UpdatePrepare(void* self, const void* params) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return 0;
  try {
    const std::uint64_t deferredJoin = Party(*impl).DeferredJoin();
    if (deferredJoin != 0) return deferredJoin;
    MaybeCreateParty(*impl, params);
  } catch (const std::exception&) {
    ReportFailure(impl, kUpdate);
  }
  return 0;
}

namespace {

EventKind KindOf(SocialParty::EventKind kind, bool* deliver) {
  using Kind = SocialParty::EventKind;
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

void UpdateCollect(void* self, EventBatch* out) noexcept {
  if (out == nullptr) return;
  out->count = 0;
  out->dropped = 0;
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return;
  try {
    PublishView(*impl);
    SyncObject(*impl, *CurrentView(*impl));
    for (const SocialParty::Event& event : Party(*impl).DrainEvents()) {
      bool deliver = false;
      const EventKind kind = KindOf(event.kind, &deliver);
      if (!deliver) continue;
      if (out->count >= kMaxPendingEvents) {
        ++out->dropped;
        continue;
      }
      PendingEvent& slot = out->events[out->count++];
      slot.kind = kind;
      slot.index = event.index;
      slot.code = event.code;
      slot.id = event.id;
      std::strncpy(slot.name, event.name.c_str(), kEventNameBytes - 1);
      slot.name[kEventNameBytes - 1] = '\0';
    }
    if (out->dropped != 0) {
      LogFields(LogLevel::kError, "social_events_dropped", {{"dropped", out->dropped}, {"capacity", kMaxPendingEvents}});
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
    SocialParty::State& party = Party(*impl);
    if (party.BeginJoin(partyId)) return JoinStep::kAskGate;
    LogFields(LogLevel::kInfo, "social_join",
              {{"party", static_cast<long long>(partyId)}, {"result", "deferred"},
               {"deferred_party", static_cast<long long>(party.DeferredJoin())}});
  } catch (const std::exception&) {
    ReportFailure(impl, kJoinInternal);
  }
  return JoinStep::kDeferred;
}

void JoinFinish(void* self, std::uint64_t partyId, bool allowed) noexcept {
  Impl* impl = OwnerOf(self);
  if (impl == nullptr) return;
  try {
    SocialParty::State& party = Party(*impl);
    if (!allowed) {
      party.AbandonJoin(partyId);
      LogFields(LogLevel::kInfo, "social_join", {{"party", static_cast<long long>(partyId)}, {"result", "declined"}});
      return;
    }
    SendParty(*impl, "party join", party.Join(partyId));
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
  ResetBase(*impl_);
  Put<std::uint32_t>(object, kOffJoinPolicy, SocialParty::kJoinPolicyEveryone);
  Put<std::uintptr_t>(object, kOffMemberJson, reinterpret_cast<std::uintptr_t>(impl_->memberJson.data()));
}

Facade::~Facade() {
  if (impl_ != nullptr && impl_->processWide) g_destroyed.fetch_add(1, std::memory_order_relaxed);
}

void* Facade::Object() noexcept { return impl_->object.data(); }

std::uint32_t Facade::InitializeCalls() const noexcept { return impl_->initializeCalls.load(std::memory_order_relaxed); }
std::uint32_t Facade::ShutdownCalls() const noexcept { return impl_->shutdownCalls.load(std::memory_order_relaxed); }
std::uint32_t Facade::SlotFailures() const noexcept { return impl_->slotFailures.load(std::memory_order_relaxed); }
std::uint32_t Facade::CallbackCalls() const noexcept { return impl_->callbackCalls.load(std::memory_order_relaxed); }

Ports ProductionPorts() {
  Ports ports;
  ports.party = &SocialParty::Global();
  ports.friends = &SocialRoster::Global();
  ports.recent = &SocialRoster::RecentlyMet();
  ports.send = &SocialParty::Send;
  // ExitLobby stores NRadEngine::SUuid::kInvalid, a global the game initialises at startup; the pointer
  // is read at call time. Not found (a host test) is sixteen zero bytes.
  ports.invalidUuid = static_cast<const std::uint8_t*>(dlsym(RTLD_DEFAULT, "_ZN10NRadEngine5SUuid8kInvalidE"));
  return ports;
}

void SetLocalAccount(std::uint64_t accountId, const char* displayName) {
  SocialParty::Global().SetSelf(accountId, displayName != nullptr ? std::string(displayName) : std::string());
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
