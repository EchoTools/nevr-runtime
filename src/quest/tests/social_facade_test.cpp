// The Quest social facade, driven the way the game drives it: through the vtable of the object it was
// handed, with frames the NEVR service sends fed in through ObserveFrames. Host-only, no game.
//
// Run: social_facade_test

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_facade.h"
#include "quest/social/social_frames.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/social_names.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"

namespace {

using namespace quest_social;

// ---- harness --------------------------------------------------------------------------------

std::vector<SocialParty::Message> g_sent;
std::vector<std::string> g_lines;
bool g_sendOk = true;
bool g_sendThrows = false;
std::uint64_t g_now = 1000;

bool RecordingSend(const std::vector<SocialParty::Message>& messages) {
  if (g_sendThrows) throw std::runtime_error("send failed");
  for (const SocialParty::Message& m : messages) g_sent.push_back(m);
  return g_sendOk;
}

std::uint64_t Clock() { return g_now; }

struct World {
  SocialParty::State party;
  SocialRoster::Roster friends;
  SocialRoster::RecentList recent;
  Ports ports;
  std::unique_ptr<Facade> facade;

  World() {
    ports.party = &party;
    ports.friends = &friends;
    ports.recent = &recent;
    ports.send = &RecordingSend;
    ports.nowSeconds = &Clock;
    facade = std::make_unique<Facade>(ports);
    g_sent.clear();
    g_sendOk = true;
    g_sendThrows = false;
    g_now = 1000;
    SocialNames::GlobalResolver().Reset();
    ResetFacadeCountersForTest();
    g_lines.clear();
  }
  void* Obj() { return facade->Object(); }
};

template <typename Fn>
Fn SlotFn(void* object, std::size_t index) {
  const std::uintptr_t* vtable = nullptr;
  std::memcpy(&vtable, object, sizeof(vtable));
  Fn fn = nullptr;
  std::memcpy(&fn, &vtable[index], sizeof(fn));
  return fn;
}

using Void0 = void (*)(void*);
using U32_0 = std::uint32_t (*)(void*);
using U32_U32 = std::uint32_t (*)(void*, std::uint32_t);
using U64_0 = std::uint64_t (*)(void*);
using U64_U32 = std::uint64_t (*)(void*, std::uint32_t);
using Str_U32 = const char* (*)(void*, std::uint32_t);
using Void_U32 = void (*)(void*, std::uint32_t);
using Void_U64 = void (*)(void*, std::uint64_t);

std::uint32_t Get32(void* object, std::size_t offset) {
  std::uint32_t v = 0;
  std::memcpy(&v, static_cast<std::uint8_t*>(object) + offset, sizeof(v));
  return v;
}
std::uint64_t Get64(void* object, std::size_t offset) {
  std::uint64_t v = 0;
  std::memcpy(&v, static_cast<std::uint8_t*>(object) + offset, sizeof(v));
  return v;
}

std::string Le(std::uint64_t value, int bytes) {
  std::string out;
  SocialParty::AppendLe(out, value, bytes);
  return out;
}

void Feed(World& w, std::uint64_t symbol, const std::string& payload) {
  SocialParty::Message m;
  m.symbol = symbol;
  m.payload = payload;
  const std::string frame = SocialParty::Frame(m);
  ObserveFrames(w.ports, Direction::kServerToGame, reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size(), g_now);
}

void FeedParty(World& w, const char* reply, std::uint64_t a, std::uint64_t b) {
  Feed(w, SocialParty::ReplySymbol(reply), Le(a, 8) + Le(b, 8));
}

// ---- the game's callbacks -------------------------------------------------------------------

struct Recorded {
  std::vector<std::string> calls;
  std::uint32_t gate = 1;
};
Recorded g_rec;

std::size_t IndexOf(const void* buffer) {
  std::size_t index = 0;
  std::memcpy(&index, buffer, sizeof(index));
  return index;
}
void CbVoid(void*, const void* buffer) { g_rec.calls.push_back("v" + std::to_string(IndexOf(buffer))); }
void CbU32(void*, const void* buffer, std::uint32_t value) {
  g_rec.calls.push_back("u" + std::to_string(IndexOf(buffer)) + ":" + std::to_string(value));
}
void CbIdName(void*, const void* buffer, std::uint64_t id, const char* name) {
  g_rec.calls.push_back("n" + std::to_string(IndexOf(buffer)) + ":" + std::to_string(id) + ":" + name);
}
std::uint32_t CbGate(void*, const void* buffer, std::uint32_t, std::uint32_t) {
  g_rec.calls.push_back("g" + std::to_string(IndexOf(buffer)));
  return g_rec.gate;
}

std::array<std::uint8_t, kCallbackBytes> MakeCallbacks() {
  std::array<std::uint8_t, kCallbackBytes> bytes{};
  const auto put = [&](std::size_t index, auto fn) {
    std::uint8_t* base = bytes.data() + kCallbackStride * index;
    const std::uintptr_t function = reinterpret_cast<std::uintptr_t>(fn);
    std::memcpy(base + 8, &index, sizeof(index));  // the inline buffer carries the callback's index
    std::memcpy(base + 0x18, &function, sizeof(function));
  };
  for (std::size_t i : {kCbCreated, kCbJoined, kCbUpdated, kCbHostChanged, kCbLeft, kCbKicked, kCbFriendsRefreshed})
    put(i, &CbVoid);
  for (std::size_t i : {kCbJoinFailed, kCbMemberJoined, kCbMemberUpdated, kCbInviteReceived}) put(i, &CbU32);
  put(kCbMemberLeft, &CbIdName);
  put(kCbInviteAccepted, &CbGate);
  return bytes;
}

void Init(World& w, const std::array<std::uint8_t, kCallbackBytes>& callbacks) {
  using InitFn = std::int32_t (*)(void*, std::uint32_t, const void*);
  QCHECK(SlotFn<InitFn>(w.Obj(), kInitialize)(w.Obj(), 10, callbacks.data()) == 0);
}

void Update(World& w, std::uint8_t flags) {
  using UpdateFn = void (*)(void*, const void*);
  SlotFn<UpdateFn>(w.Obj(), kUpdate)(w.Obj(), &flags);
}

bool Called(const std::string& call) {
  for (const std::string& c : g_rec.calls)
    if (c == call) return true;
  return false;
}

constexpr std::uint64_t kSelf = 1001;

// A party of one led by the local user.
void CreateParty(World& w, std::uint64_t partyId) {
  w.party.SetSelf(kSelf, "alice");
  Update(w, 1);  // the game asks for a party
  FeedParty(w, "PartyCreateSuccess", partyId, kSelf);
  Update(w, 0);
}

void CaptureLog(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

std::size_t CountLines(const char* needle) {
  std::size_t n = 0;
  for (const std::string& l : g_lines) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

// ---- tests ----------------------------------------------------------------------------------

void TestObjectShape() {
  World w;
  void* obj = w.Obj();
  QCHECK(obj != nullptr);
  const std::uintptr_t* vtable = nullptr;
  std::memcpy(&vtable, obj, sizeof(vtable));
  QCHECK(vtable != nullptr);
  for (std::size_t i = 0; i < kSlotCount; ++i) QCHECK(vtable[i] != 0);
  for (std::size_t i = 1; i < kSlotCount; ++i) QCHECK(vtable[i] != vtable[i - 1]);  // one entry per slot
  QCHECK(Get64(obj, kOffOwner) != 0);
  QCHECK(Get32(obj, kOffJoinPolicy) == SocialParty::kJoinPolicyEveryone);
  QCHECK(Get64(obj, kOffMemberJson) != 0);
  // The base reset state: state word (state & ~1) | 2, no lobby, no members.
  QCHECK((Get32(obj, kOffFlags) & kFlagJoinable) != 0);
  QCHECK(Get32(obj, kOffLocalCount) == 0 && Get32(obj, kOffMemberCount) == 0);
  QCHECK(Get64(obj, kOffLobbyMatchType) == UINT64_MAX);
}

void TestInitializeAndShutdown() {
  World w;
  const auto callbacks = MakeCallbacks();
  Init(w, callbacks);
  QCHECK(w.facade->InitializeCalls() == 1);
  QCHECK(std::memcmp(static_cast<std::uint8_t*>(w.Obj()) + 8, callbacks.data(), kCallbackBytes) == 0);
  SlotFn<Void0>(w.Obj(), kShutdown)(w.Obj());
  QCHECK(w.facade->ShutdownCalls() == 1);
  std::array<std::uint8_t, kCallbackBytes> zero{};
  QCHECK(std::memcmp(static_cast<std::uint8_t*>(w.Obj()) + 8, zero.data(), kCallbackBytes) == 0);
  // After shutdown a party event reaches no game callback.
  g_rec.calls.clear();
  w.party.SetSelf(kSelf, "alice");
  FeedParty(w, "PartyCreateSuccess", 5, kSelf);
  Update(w, 0);
  QCHECK(g_rec.calls.empty());
}

void TestFriendRoster() {
  World w;
  w.party.SetSelf(kSelf, "alice");
  // SNSFriendListResponse: header(8) + offline, busy, online, sent, recv, reserved (u32 each).
  Feed(w, kSymFriendListResponse, Le(0, 8) + Le(1, 4) + Le(0, 4) + Le(1, 4) + Le(0, 4) + Le(0, 4) + Le(0, 4));
  // SNSFriendStatusNotify: header(8) + id(8) + status(1) + 7 reserved; 0 online, 2 offline.
  Feed(w, kSymFriendStatusNotify, Le(0, 8) + Le(2002, 8) + Le(2, 1) + Le(0, 7));
  Feed(w, kSymFriendStatusNotify, Le(0, 8) + Le(3003, 8) + Le(0, 1) + Le(0, 7));
  void* obj = w.Obj();
  QCHECK(SlotFn<U32_0>(obj, kFriendCount)(obj) == 2);
  QCHECK(SlotFn<U32_0>(obj, kOnlineFriendCount)(obj) == 1);
  QCHECK(SlotFn<U32_0>(obj, kOfflineFriendCount)(obj) == 1);
  // Online friends first; FriendStatus is 2 online, 0 offline.
  QCHECK(SlotFn<U64_U32>(obj, kFriendId)(obj, 0) == 3003);
  QCHECK(SlotFn<U64_U32>(obj, kFriendId)(obj, 1) == 2002);
  QCHECK(SlotFn<U32_U32>(obj, kFriendStatus)(obj, 0) == 2);
  QCHECK(SlotFn<U32_U32>(obj, kFriendStatus)(obj, 1) == 0);
  QCHECK(SlotFn<U64_U32>(obj, kFriendId)(obj, 7) == 0);  // out of range answers 0
  // Each friend's name was asked for once.
  int profileRequests = 0;
  for (const SocialParty::Message& m : g_sent) profileRequests += m.symbol == SocialNames::kProfileRequest ? 1 : 0;
  QCHECK(profileRequests == 0);  // no profile decoder is registered, so no reply could be read: none asked for
  // The friend tab refresh sends the refresh request.
  g_sent.clear();
  SlotFn<Void0>(obj, kRefreshFriends)(obj);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kFriendListRefreshRequest);
  // A friend change from the server asks for the list again.
  g_sent.clear();
  Feed(w, 0xc237c84c31d3ae05ULL, Le(0, 8) + Le(2002, 8));  // SNSFriendAcceptNotify
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kFriendListRefreshRequest);
}

void TestPartyCreateAndSlots() {
  World w;
  const auto callbacks = MakeCallbacks();
  g_rec.calls.clear();
  Init(w, callbacks);
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  Update(w, 1);  // flags bit 0: the game wants a party
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kCreateRequest);
  g_now += 1;
  Update(w, 1);  // a create is in flight: no second request, whatever the clock says
  QCHECK(g_sent.size() == 1);
  FeedParty(w, "PartyCreateSuccess", 777, kSelf);
  Update(w, 0);
  QCHECK(Called("v" + std::to_string(kCbCreated)));
  QCHECK(SlotFn<U32_0>(obj, kReady)(obj) == 1);
  QCHECK(SlotFn<U64_0>(obj, kId)(obj) == 777);
  QCHECK(SlotFn<U64_0>(obj, kHost)(obj) == kSelf);
  QCHECK(SlotFn<U32_0>(obj, kIsHost)(obj) == 1);
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == 1);
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(obj, 0) == kSelf);
  QCHECK(std::string(SlotFn<Str_U32>(obj, kMemberName)(obj, 0)) == "alice");
  QCHECK(std::string(SlotFn<Str_U32>(obj, kMemberName)(obj, 5)).empty());
  QCHECK(Get64(obj, kOffRoomId) == 777);
  QCHECK(Get32(obj, kOffMemberCount) == 1 && Get32(obj, kOffLocalCount) == 1);
  QCHECK(Get32(obj, kOffMaxMembers) == kPartyMaxMembers);
  QCHECK(SlotFn<U64_U32>(obj, kLocalId)(obj, 0) == 0);
  QCHECK(SlotFn<U64_U32>(obj, kLocalId)(obj, 1) == 0xFFFFFFFFULL);  // pnsovr's value: 32-bit -1, zero-extended
  QCHECK(SlotFn<U64_U32>(obj, kMemberDataWritable)(obj, 0) == 0);
}

void TestNoCreateBeforeLogin() {
  World w;
  Update(w, 1);  // the game wants a party, but no account is signed in
  QCHECK(g_sent.empty());
  w.party.SetSelf(kSelf, "alice");
  Update(w, 1);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kCreateRequest);
}

void TestCreateRetriesAfterInterval() {
  World w;
  w.party.SetSelf(kSelf, "alice");
  Update(w, 1);
  QCHECK(g_sent.size() == 1);
  FeedParty(w, "PartyCreateFailure", 0, 0);  // the server refused; the game still wants a party
  g_now += 2;
  Update(w, 1);
  QCHECK(g_sent.size() == 1);  // inside the four-second retry interval
  g_now += 3;
  Update(w, 1);
  QCHECK(g_sent.size() == 2);
}

void TestMembersJoinAndLeave() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  void* obj = w.Obj();
  g_rec.calls.clear();
  FeedParty(w, "PartyJoinNotify", 777, 3003);
  Update(w, 0);
  QCHECK(Called("u" + std::to_string(kCbMemberJoined) + ":1"));
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == 2);
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(obj, 1) == 3003);
  QCHECK(Get32(obj, kOffMemberCount) == 2);
  g_rec.calls.clear();
  FeedParty(w, "PartyLeaveNotify", 777, 3003);
  Update(w, 0);
  QCHECK(Called("n" + std::to_string(kCbMemberLeft) + ":3003:3003"));
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == 1);
  // Pass ownership to a member: host changes, the request goes out.
  FeedParty(w, "PartyJoinNotify", 777, 4004);
  Update(w, 0);
  g_sent.clear();
  SlotFn<Void_U32>(obj, kPassOwnership)(obj, 1);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kPassRequest);
  Update(w, 0);
  QCHECK(SlotFn<U64_0>(obj, kHost)(obj) == 4004);
  QCHECK(SlotFn<U32_0>(obj, kIsHost)(obj) == 0);
  QCHECK(Get32(obj, kOffOwnerIndex) == 1);
}

void TestInvitesAndJoin() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  g_rec.calls.clear();
  FeedParty(w, "PartyInviteNotify", 555, 2002);
  Update(w, 0);
  QCHECK(Called("u" + std::to_string(kCbInviteReceived) + ":0"));
  QCHECK(SlotFn<U32_0>(obj, kInviteCount)(obj) == 1);
  QCHECK(std::string(SlotFn<Str_U32>(obj, kInviteSender)(obj, 0)) == "2002");
  QCHECK(SlotFn<U64_U32>(obj, kInviteSentTime)(obj, 0) == g_now);

  // The accept gate declines: nothing is sent and the join is forgotten.
  g_rec.gate = 0;
  g_sent.clear();
  SlotFn<Void_U32>(obj, kAcceptInvite)(obj, 0);
  QCHECK(Called("g" + std::to_string(kCbInviteAccepted)));
  QCHECK(g_sent.empty());

  // The gate allows it: the join answers the invite, targeted at the inviter.
  FeedParty(w, "PartyInviteNotify", 556, 2002);
  Update(w, 0);
  g_rec.gate = 1;
  SlotFn<Void_U32>(obj, kAcceptInvite)(obj, 0);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kInviteResponse);
  Update(w, 0);  // the slots read the view the last Update published
  QCHECK(SlotFn<U32_0>(obj, kInviteCount)(obj) == 0);

  // The join succeeds: Joined, then a MemberJoined for the inviter.
  g_rec.calls.clear();
  FeedParty(w, "PartyJoinSuccess", 556, 2002);
  Update(w, 0);
  QCHECK(Called("v" + std::to_string(kCbJoined)));
  QCHECK(Called("u" + std::to_string(kCbMemberJoined) + ":1"));
  QCHECK(SlotFn<U64_0>(obj, kId)(obj) == 556);
  QCHECK(SlotFn<U64_0>(obj, kHost)(obj) == 2002);

  // Dismissing an invite sends the decline (param 0).
  FeedParty(w, "PartyInviteNotify", 600, 7007);
  Update(w, 0);
  g_sent.clear();
  SlotFn<Void_U32>(obj, kDismissInvite)(obj, 0);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kInviteResponse);
}

void TestFriendInvitable() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  Feed(w, kSymFriendListResponse, Le(0, 8) + Le(0, 4) + Le(0, 4) + Le(2, 4) + Le(0, 4) + Le(0, 4) + Le(0, 4));
  Feed(w, kSymFriendStatusNotify, Le(0, 8) + Le(2002, 8) + Le(0, 1) + Le(0, 7));
  Feed(w, kSymFriendStatusNotify, Le(0, 8) + Le(3003, 8) + Le(0, 1) + Le(0, 7));
  void* obj = w.Obj();
  QCHECK(SlotFn<U32_U32>(obj, kFriendIsInvitable)(obj, 0) == 0);  // no party yet: nobody is invitable
  CreateParty(w, 777);
  QCHECK(SlotFn<U32_U32>(obj, kFriendIsInvitable)(obj, 0) == 1);
  QCHECK(SlotFn<U32_U32>(obj, kFriendIsInvitable)(obj, 1) == 1);
  FeedParty(w, "PartyJoinNotify", 777, 2002);  // a friend is now in the party
  Update(w, 0);
  std::uint32_t invitable = 0;
  for (std::uint32_t i = 0; i < 2; ++i) {
    if (SlotFn<U64_U32>(obj, kFriendId)(obj, i) == 2002) {
      QCHECK(SlotFn<U32_U32>(obj, kFriendIsInvitable)(obj, i) == 0);
    } else {
      invitable += SlotFn<U32_U32>(obj, kFriendIsInvitable)(obj, i);
    }
  }
  QCHECK(invitable == 1);
  // Not a friend row at all.
  QCHECK(SlotFn<U32_U32>(obj, kFriendIsInvitable)(obj, 99) == 0);
}

void TestSendingFromSlots() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  void* obj = w.Obj();
  g_sent.clear();
  SlotFn<Void_U64>(obj, kSendInviteInternal)(obj, 2002);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kInviteRequest);
  QCHECK(g_sent[0].payload.size() >= 0x28 && std::memcmp(g_sent[0].payload.data() + 0x20, "\xd2\x07", 2) == 0);  // 2002 LE
  g_sent.clear();
  using OpenFriend = void (*)(void*, std::uint32_t, std::uint64_t);
  SlotFn<OpenFriend>(obj, kOpenFriendRequestUI)(obj, 0, 9009);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kFriendInviteRequest);
  g_sent.clear();
  SlotFn<Void_U32>(obj, kSetJoinPolicy)(obj, 1);
  QCHECK(Get32(obj, kOffJoinPolicy) == 1);
  QCHECK(SlotFn<U32_0>(obj, kJoinPolicy)(obj) == 1);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kSetJoinPolicyRequest);
  // Lock and unlock follow SetJoinableInternal.
  g_sent.clear();
  SlotFn<Void_U32>(obj, kSetJoinableInternal)(obj, 0);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kLockRequest);
  // A sender that reports failure is logged and does not disturb the game (the slot still returns).
  g_sendOk = false;
  SlotFn<Void0>(obj, kRefreshFriends)(obj);
  QCHECK(w.facade->SlotFailures() == 0);
  // Leave sends only when there is someone else; Reset leaves silently and zeroes the counts.
  g_sendOk = true;
  g_sent.clear();
  SlotFn<Void0>(obj, kLeave)(obj);
  QCHECK(g_sent.empty());  // a party of one is not left
  SlotFn<Void0>(obj, kReset)(obj);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kLeaveRequest);
  QCHECK(Get32(obj, kOffLocalCount) == 0 && Get32(obj, kOffMemberCount) == 0);
  SlotFn<Void_U32>(obj, kAddMember)(obj, 3);  // only local user 0 exists
  QCHECK(Get32(obj, kOffLocalCount) == 0 && Get32(obj, kOffMemberCount) == 0);
  SlotFn<Void_U32>(obj, kAddMember)(obj, 0);
  QCHECK(Get32(obj, kOffLocalCount) == 1 && Get32(obj, kOffMemberCount) == 1);
}

void TestLobbyFields() {
  World w;
  void* obj = w.Obj();
  const std::uint8_t uuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  using EnterOnline = void (*)(void*, const void*, std::uint64_t, std::uint16_t, std::uint8_t);
  using EnterOffline = void (*)(void*, const void*, std::uint64_t, std::uint8_t);
  using EnterFull = void (*)(void*, const void*, std::uint64_t, std::uint16_t, std::uint8_t, std::uint32_t);
  SlotFn<EnterOnline>(obj, kEnterOnlineLobby)(obj, uuid, 0x1122334455667788ULL, 3, 1);
  QCHECK(std::memcmp(static_cast<std::uint8_t*>(obj) + kOffLobbyUuid, uuid, 16) == 0);
  QCHECK(Get64(obj, kOffLobbyMatchType) == 0x1122334455667788ULL);
  std::uint16_t team = 0;
  std::memcpy(&team, static_cast<std::uint8_t*>(obj) + kOffLobbyTeam, 2);
  QCHECK(team == 3);
  QCHECK(static_cast<std::uint8_t*>(obj)[kOffLobbyType] == 1);
  QCHECK((Get32(obj, kOffFlags) & kFlagOfflineLobby) == 0);
  SlotFn<EnterOffline>(obj, kEnterOfflineLobby)(obj, uuid, 5, 2);
  std::memcpy(&team, static_cast<std::uint8_t*>(obj) + kOffLobbyTeam, 2);
  QCHECK(team == UINT16_MAX);
  QCHECK((Get32(obj, kOffFlags) & kFlagOfflineLobby) != 0);
  SlotFn<EnterFull>(obj, kEnterLobby)(obj, uuid, 6, 4, 1, 0);
  QCHECK((Get32(obj, kOffFlags) & kFlagOfflineLobby) == 0);
  SlotFn<Void0>(obj, kExitLobby)(obj);
  QCHECK(Get64(obj, kOffLobbyMatchType) == UINT64_MAX);
  QCHECK(static_cast<std::uint8_t*>(obj)[kOffLobbyType] == 2);
  const std::uint8_t zero[16] = {};
  QCHECK(std::memcmp(static_cast<std::uint8_t*>(obj) + kOffLobbyUuid, zero, 16) == 0);
}

void TestRecentlyMet() {
  World w;
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  QCHECK(SlotFn<U32_0>(obj, kRefreshingRecentlyMetUsers)(obj) == 0);
  SlotFn<Void0>(obj, kRefreshRecentlyMetUsers)(obj);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kRecentlyMetRefreshRequest);
  QCHECK(SlotFn<U32_0>(obj, kRefreshingRecentlyMetUsers)(obj) == 1);
  // A refresh whose request could not be sent ends at once, so the game's poll does not hang.
  World failing;
  failing.party.SetSelf(kSelf, "alice");
  g_sendOk = false;
  SlotFn<Void0>(failing.Obj(), kRefreshRecentlyMetUsers)(failing.Obj());
  QCHECK(SlotFn<U32_0>(failing.Obj(), kRefreshingRecentlyMetUsers)(failing.Obj()) == 0);
}

void TestExceptionContainment() {
  World w;
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  g_sendThrows = true;
  SlotFn<Void0>(obj, kRefreshFriends)(obj);  // the sender throws; the slot must not
  QCHECK(w.facade->SlotFailures() == 1);
  g_sendThrows = false;
  // A slot called with no object answers its zero value instead of faulting.
  QCHECK(SlotFn<U32_0>(obj, kReady)(nullptr) == 0);
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(nullptr, 0) == 0);
  SlotFn<Void0>(obj, kRefreshFriends)(nullptr);
  QCHECK(w.facade->SlotFailures() == 1);
}

void TestLocalAccount() {
  QCHECK(&Facade::Instance() == &Facade::Instance());  // constructed on first use, one object
  // The process-wide model: SetLocalAccount makes the account the facade's local member.
  SetLocalAccount(4242, "bob");
  const SocialParty::View view = SocialParty::Global().Snapshot();
  QCHECK(view.selfId == 4242 && view.selfName == "bob");
  SetLocalAccount(4242, nullptr);  // no name: the id stays, the name is not invented
  QCHECK(SocialParty::Global().Snapshot().selfId == 4242);
}


// ---- a game exception passes through the facade's frames untouched ---------------------------
// The callbacks below are "game code": they throw, and the test plays the game's caller, catching
// above the slot. The slot frames between them carry no landing pad (tools/check_quest_social_frames.sh
// pins that on the object), so nothing in the facade may catch, count or swallow the exception.

struct GameError : std::runtime_error {
  GameError() : std::runtime_error("game callback failed") {}
};
void CbThrowVoid(void*, const void*) { throw GameError(); }
std::uint32_t CbGateThrows(void*, const void*, std::uint32_t, std::uint32_t) { throw GameError(); }

void SetCallback(std::array<std::uint8_t, kCallbackBytes>* bytes, std::size_t index, void* fn) {
  const std::uintptr_t function = reinterpret_cast<std::uintptr_t>(fn);
  std::memcpy(bytes->data() + kCallbackStride * index + 0x18, &function, sizeof(function));
}

void TestGameExceptionThroughUpdate() {
  World w;
  auto callbacks = MakeCallbacks();
  SetCallback(&callbacks, kCbCreated, reinterpret_cast<void*>(&CbThrowVoid));
  Init(w, callbacks);
  w.party.SetSelf(kSelf, "alice");
  FeedParty(w, "PartyCreateSuccess", 777, kSelf);  // queues a Created event, delivered by the next Update
  using UpdateFn = void (*)(void*, const void*);
  const std::uint8_t flags = 0;
  bool caught = false;
  try {
    SlotFn<UpdateFn>(w.Obj(), kUpdate)(w.Obj(), &flags);
  } catch (const GameError&) {
    caught = true;
  }
  QCHECK(caught);                         // the game's exception reached the game's caller
  QCHECK(w.facade->SlotFailures() == 0);  // and the facade neither caught nor counted it
  // The facade is intact afterwards: the next Update runs and the slots answer.
  g_rec.calls.clear();
  SlotFn<UpdateFn>(w.Obj(), kUpdate)(w.Obj(), &flags);
  QCHECK(SlotFn<U64_0>(w.Obj(), kId)(w.Obj()) == 777);
}

void TestGameExceptionThroughGate() {
  World w;
  auto callbacks = MakeCallbacks();
  SetCallback(&callbacks, kCbInviteAccepted, reinterpret_cast<void*>(&CbGateThrows));
  Init(w, callbacks);
  w.party.SetSelf(kSelf, "alice");
  FeedParty(w, "PartyInviteNotify", 555, 2002);
  Update(w, 0);
  bool caught = false;
  try {
    SlotFn<Void_U32>(w.Obj(), kAcceptInvite)(w.Obj(), 0);
  } catch (const GameError&) {
    caught = true;
  }
  QCHECK(caught);
  QCHECK(w.facade->SlotFailures() == 0);
  caught = false;
  try {
    SlotFn<Void_U64>(w.Obj(), kJoinInternal)(w.Obj(), 556);
  } catch (const GameError&) {
    caught = true;
  }
  QCHECK(caught);
  QCHECK(w.facade->SlotFailures() == 0);
}

void TestEventBatchCarriesTheRemainder() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  g_rec.calls.clear();
  // 40 senders, one invite each: 40 InviteReceived events in one frame; a batch holds 32.
  for (std::uint64_t sender = 3000; sender < 3040; ++sender) FeedParty(w, "PartyInviteNotify", 9000 + sender, sender);
  const auto received = [] {
    std::size_t n = 0;
    for (const std::string& c : g_rec.calls) n += c == "u" + std::to_string(kCbInviteReceived) + ":0" ? 1 : 0;
    return n;
  };
  Update(w, 0);
  QCHECK(received() == 32);
  Update(w, 0);  // the remainder is carried to the next frame, not dropped
  QCHECK(received() == 40);
  QCHECK(FacadeCountersView().eventsDropped.load() == 0);
  QCHECK(SlotFn<U32_0>(w.Obj(), kInviteCount)(w.Obj()) == 40);
}

void TestEventQueueOverflowIsCounted() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  g_rec.calls.clear();
  // 300 events in one frame: the carry queue holds 256, the newest 44 are dropped and counted.
  for (std::uint64_t sender = 4000; sender < 4300; ++sender) FeedParty(w, "PartyInviteNotify", 9000 + sender, sender);
  for (int i = 0; i < 12; ++i) Update(w, 0);
  QCHECK(g_rec.calls.size() == 256);
  QCHECK(FacadeCountersView().eventsDropped.load() == 44);
  QCHECK(CountLines("social_events_dropped") == 1);  // logged once, counted always
}

void TestMemberCountNeverExceedsTheGamesArray() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);  // the local user alone: 1 member
  void* obj = w.Obj();
  for (std::uint64_t id = 3000; id < 3008; ++id) FeedParty(w, "PartyJoinNotify", 777, id);  // 9 members
  Update(w, 0);
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == 9);
  QCHECK(Get32(obj, kOffMemberCount) == 9);
  FeedParty(w, "PartyJoinNotify", 777, 3008);  // 10: exactly the array
  Update(w, 0);
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == kMemberJsonSlots);
  QCHECK(Get32(obj, kOffMemberCount) == kMemberJsonSlots);
  QCHECK(FacadeCountersView().membersClamped.load() == 0);
  FeedParty(w, "PartyJoinNotify", 777, 3009);  // 11: one past it
  Update(w, 0);
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == kMemberJsonSlots);
  QCHECK(Get32(obj, kOffMemberCount) == kMemberJsonSlots);
  QCHECK(FacadeCountersView().membersClamped.load() >= 1);
  for (std::uint64_t id = 3010; id < 3013; ++id) FeedParty(w, "PartyJoinNotify", 777, id);  // 14
  Update(w, 0);
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == kMemberJsonSlots);
  QCHECK(Get32(obj, kOffMemberCount) == kMemberJsonSlots);
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(obj, kMemberJsonSlots - 1) != 0);
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(obj, kMemberJsonSlots) == 0);  // past the array: answered, never read
  QCHECK(std::string(SlotFn<Str_U32>(obj, kMemberName)(obj, 12)).empty());
}

void TestFailedSendsDoNotStickTheModel() {
  // Create: the sender refuses, so the model must not stay "creating"; a working sender then creates.
  World w;
  w.party.SetSelf(kSelf, "alice");
  g_sendOk = false;
  Update(w, 1);
  QCHECK(g_sent.size() == 1);  // attempted
  QCHECK(!w.party.Snapshot().creating);
  QCHECK(FacadeCountersView().sendFailed.load() == 1);
  g_sendOk = true;
  g_now += 5;  // past the retry interval
  Update(w, 1);
  QCHECK(g_sent.size() == 2 && g_sent[1].symbol == SocialParty::kCreateRequest);
  QCHECK(w.party.Snapshot().creating);  // now genuinely in flight
  // ... and after the server answers, a join is not deferred forever.
  FeedParty(w, "PartyCreateSuccess", 777, kSelf);
  Update(w, 0);

  // Invite with no party: the create it implies must roll back when it cannot be sent.
  World v;
  v.party.SetSelf(kSelf, "alice");
  g_sendOk = false;
  SlotFn<Void_U64>(v.Obj(), kSendInviteInternal)(v.Obj(), 2002);
  QCHECK(!v.party.Snapshot().creating);
  g_sendOk = true;
  g_sent.clear();
  SlotFn<Void_U64>(v.Obj(), kSendInviteInternal)(v.Obj(), 2002);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kCreateRequest);

  // Join: the request is refused, so the model is not left "joining".
  World j;
  j.party.SetSelf(kSelf, "alice");
  Init(j, MakeCallbacks());
  g_sendOk = false;
  SlotFn<Void_U64>(j.Obj(), kJoinInternal)(j.Obj(), 556);
  QCHECK(!j.party.Snapshot().joining);
  g_sendOk = true;
  g_sent.clear();
  SlotFn<Void_U64>(j.Obj(), kJoinInternal)(j.Obj(), 556);
  QCHECK(g_sent.size() == 1 && j.party.Snapshot().joining);

  // Lock: a refused lock request is asked again.
  World l;
  Init(l, MakeCallbacks());
  CreateParty(l, 777);
  g_sendOk = false;
  SlotFn<Void_U32>(l.Obj(), kSetJoinableInternal)(l.Obj(), 0);
  g_sendOk = true;
  g_sent.clear();
  SlotFn<Void_U32>(l.Obj(), kSetJoinableInternal)(l.Obj(), 0);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kLockRequest);
}

void TestDeferredJoinLogsOncePerParty() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  Update(w, 1);  // a create is now in flight, unanswered
  g_lines.clear();
  SlotFn<Void_U64>(w.Obj(), kJoinInternal)(w.Obj(), 556);  // deferred behind the create
  for (int i = 0; i < 100; ++i) Update(w, 0);              // the model retries it every frame
  QCHECK(CountLines("\"result\":\"deferred\"") == 1);
  QCHECK(FacadeCountersView().joinDeferred.load() >= 100);
}

void TestResetKeepsTheGamesPartyJson() {
  World w;
  void* obj = w.Obj();
  std::uint8_t pattern[16];
  for (int i = 0; i < 16; ++i) pattern[i] = static_cast<std::uint8_t>(0xA0 + i);
  std::memcpy(static_cast<std::uint8_t*>(obj) + kOffPartyJson, pattern, 16);  // the game's tree pointers
  SlotFn<Void0>(obj, kReset)(obj);
  QCHECK(std::memcmp(static_cast<std::uint8_t*>(obj) + kOffPartyJson, pattern, 16) == 0);  // not zeroed, not leaked
}

void TestMemberCountAgreesBeforeLogin() {
  World w;  // no account signed in
  void* obj = w.Obj();
  SlotFn<Void_U32>(obj, kAddMember)(obj, 0);
  const std::uint32_t reported = SlotFn<U32_0>(obj, kMemberCount)(obj);
  QCHECK(reported == 1);
  QCHECK(reported == Get32(obj, kOffMemberCount));
  QCHECK(reported - Get32(obj, kOffLocalCount) == 0);  // PlatformPurchaseSucceededCB's MemberCount - [+0x200]
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(obj, 0) == 0);
}

void TestSendLogsStableIds() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  g_lines.clear();
  SlotFn<Void_U64>(w.Obj(), kSendInviteInternal)(w.Obj(), 2002);
  QCHECK(CountLines("\"name\":\"PartyInviteRequest\"") == 1);
  QCHECK(CountLines("\"arg\":2002") == 1);
}

void TestNamesAreAskedForOnlyWithADecoder() {
  World w;
  w.party.SetSelf(kSelf, "alice");
  SocialNames::SetDecoder([](const std::uint8_t*, std::size_t, std::uint64_t* id, std::string* name) {
    *id = 2002;
    *name = "Zed";
    return true;
  });
  Feed(w, kSymFriendListResponse, Le(0, 8) + Le(0, 4) + Le(0, 4) + Le(1, 4) + Le(0, 4) + Le(0, 4) + Le(0, 4));
  Feed(w, kSymFriendStatusNotify, Le(0, 8) + Le(2002, 8) + Le(0, 1) + Le(0, 7));
  int profileRequests = 0;
  for (const SocialParty::Message& m : g_sent) profileRequests += m.symbol == SocialNames::kProfileRequest ? 1 : 0;
  QCHECK(profileRequests == 1);
  Feed(w, SocialNames::kProfileSuccess, Le(0, 16));
  void* obj = w.Obj();
  QCHECK(std::string(SlotFn<Str_U32>(obj, kFriendName)(obj, 0)) == "Zed");
  SocialNames::SetDecoder(nullptr);
}

void CheckInstanceSurvivesExit() {
  // Registered before Instance() is first called, so it runs after any destructor of the instance would.
  if (Facade::ProcessWideDestroyedCountForTest() != 0) {
    std::fprintf(stderr, "social_facade_test: the process-wide Facade was destroyed at exit\n");
    std::_Exit(3);
  }
  std::printf("social_facade_test: the process-wide Facade survives process exit\n");
}

void TestFrameWalker() {
  World w;
  w.party.SetSelf(kSelf, "alice");
  // Two messages in one transport frame, the second truncated: the first is applied, the walk stops.
  SocialParty::Message a{SocialParty::ReplySymbol("PartyCreateSuccess"), Le(10, 8) + Le(kSelf, 8)};
  SocialParty::Message b{SocialParty::ReplySymbol("PartyJoinNotify"), Le(10, 8) + Le(77, 8)};
  std::string frame = SocialParty::Frame(a) + SocialParty::Frame(b);
  frame.resize(frame.size() - 3);
  const FrameStats stats = ObserveFrames(w.ports, Direction::kServerToGame, reinterpret_cast<const std::uint8_t*>(frame.data()),
                                         frame.size(), g_now);
  QCHECK(stats.messages == 1 && stats.consumed == 1 && stats.malformed == 1);
  // A bad marker stops the walk; client-to-server frames are logged, never applied.
  std::string bad = SocialParty::Frame(a);
  bad[0] = 0;
  QCHECK(ObserveFrames(w.ports, Direction::kServerToGame, reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size(), g_now)
             .malformed == 1);
  World other;
  other.party.SetSelf(kSelf, "alice");
  const std::string good = SocialParty::Frame(a);
  const FrameStats up = ObserveFrames(other.ports, Direction::kGameToServer, reinterpret_cast<const std::uint8_t*>(good.data()),
                                      good.size(), g_now);
  QCHECK(up.messages == 1 && up.consumed == 0);
  QCHECK(other.party.Snapshot().partyId == 0);
}

}  // namespace

int main() {
  std::atexit(&CheckInstanceSurvivesExit);
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLog);
  TestObjectShape();
  TestInitializeAndShutdown();
  TestFriendRoster();
  TestPartyCreateAndSlots();
  TestNoCreateBeforeLogin();
  TestCreateRetriesAfterInterval();
  TestMembersJoinAndLeave();
  TestInvitesAndJoin();
  TestFriendInvitable();
  TestSendingFromSlots();
  TestLobbyFields();
  TestRecentlyMet();
  TestExceptionContainment();
  TestFrameWalker();
  TestGameExceptionThroughUpdate();
  TestGameExceptionThroughGate();
  TestEventBatchCarriesTheRemainder();
  TestEventQueueOverflowIsCounted();
  TestMemberCountNeverExceedsTheGamesArray();
  TestFailedSendsDoNotStickTheModel();
  TestDeferredJoinLogsOncePerParty();
  TestResetKeepsTheGamesPartyJson();
  TestMemberCountAgreesBeforeLogin();
  TestSendLogsStableIds();
  TestNamesAreAskedForOnlyWithADecoder();
  TestLocalAccount();
  sentinel::SetLogSink(previous);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "social_facade_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("social_facade_test: all checks pass\n");
  return 0;
}
