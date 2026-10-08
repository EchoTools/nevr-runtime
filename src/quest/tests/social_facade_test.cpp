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
#include "quest/social/social_request_log.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/social_names.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"

namespace {

using namespace quest_social;

// ---- harness --------------------------------------------------------------------------------

std::vector<SocialParty::Message> g_sent;
std::vector<std::uint64_t> g_sentAt;  // g_now when each entry of g_sent was handed to the sender
std::vector<std::string> g_lines;
bool g_sendOk = true;
bool g_sendThrows = false;
std::uint64_t g_now = 1000;

bool RecordingSend(const std::vector<SocialParty::Message>& messages) {
  if (g_sendThrows) throw std::runtime_error("send failed");
  for (const SocialParty::Message& m : messages) {
    g_sent.push_back(m);
    g_sentAt.push_back(g_now);
  }
  return g_sendOk;
}

std::size_t SentCount(std::uint64_t symbol) {
  std::size_t n = 0;
  for (const SocialParty::Message& m : g_sent) n += m.symbol == symbol ? 1 : 0;
  return n;
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
    g_sentAt.clear();
    g_sendOk = true;
    g_sendThrows = false;
    g_now = 1000;
    SetGameJson(GameJson{});
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

std::size_t CalledCount(const std::string& call) {
  std::size_t n = 0;
  for (const std::string& c : g_rec.calls) n += c == call ? 1 : 0;
  return n;
}

// The index argument of every PartyMemberJoinedCB the game received ("u9:<index>").
std::vector<std::size_t> JoinedIndices() {
  std::vector<std::size_t> out;
  const std::string prefix = "u" + std::to_string(kCbMemberJoined) + ":";
  for (const std::string& c : g_rec.calls) {
    if (c.compare(0, prefix.size(), prefix) == 0) out.push_back(static_cast<std::size_t>(std::stoul(c.substr(prefix.size()))));
  }
  return out;
}

// Frame times that wander the way a game's do: 1 to 7 s between Updates, never the same twice in a row.
std::uint64_t Jitter(std::size_t i) {
  static const std::uint64_t kSteps[] = {1, 3, 2, 7, 1, 5, 2, 4, 6, 1, 3};
  return kSteps[i % (sizeof(kSteps) / sizeof(kSteps[0]))];
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
  QCHECK(FacadeCountersView().cbCreated.load() == 1);  // the reporter shows a headset run PartyCreatedCB
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
  QCHECK(SlotFn<U64_U32>(obj, kMemberDataWritable)(obj, 0) == Get64(obj, kOffMemberJson));  // the local member's CJson, for the game to write
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
  QCHECK(g_sent.size() == 1);  // inside the five-second retry interval (libpnsovr 0x2045f4: cmp w8, #5)
  g_now += 2;
  Update(w, 1);
  QCHECK(g_sent.size() == 1);  // 4 s: still inside it
  g_now += 1;
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
  QCHECK(FacadeCountersView().cbMemberJoined.load() == 1);
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
  QCHECK(FacadeCountersView().cbOther.load() >= 2);  // PartyJoinedCB is counted with the other callbacks
  QCHECK(FacadeCountersView().cbOther.load() >= 1);  // the accept gate, InviteReceived, ...
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
  // 40 JoinFailed events in one frame: none can be merged or dropped, and a batch holds 32.
  for (int i = 0; i < 40; ++i) FeedParty(w, "PartyJoinFailure", 0, 3);
  const std::string failed = "u" + std::to_string(kCbJoinFailed) + ":3";
  Update(w, 0);
  QCHECK(CalledCount(failed) == 32);
  Update(w, 0);  // the remainder is carried to the next frame, not dropped
  QCHECK(CalledCount(failed) == 40);
  QCHECK(FacadeCountersView().eventsDropped.load() == 0);
}

void TestInviteNotificationsAreMerged() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  g_rec.calls.clear();
  // 300 senders, one invite each: the callback takes no argument (always 0) and reads the invite list when it
  // runs, so the 300 notifications are one callback, and every invite is still listed.
  for (std::uint64_t sender = 4000; sender < 4300; ++sender) FeedParty(w, "PartyInviteNotify", 9000 + sender, sender);
  for (int i = 0; i < 12; ++i) Update(w, 0);
  QCHECK(CalledCount("u" + std::to_string(kCbInviteReceived) + ":0") == 1);
  QCHECK(FacadeCountersView().eventsDropped.load() == 0);
  QCHECK(SlotFn<U32_0>(w.Obj(), kInviteCount)(w.Obj()) == 300);
}

// The carry queue's drop policy. 256 is the soft limit. Events that say something nothing later repeats
// (Created, MemberJoined, JoinFailed, HostChanged) are never dropped to make room; the oldest Updated or
// MemberUpdated is. Dropping the newest (the old policy) loses the tail, dropping the oldest of any kind loses
// the head: both are pinned here.
void TestEventQueueDropsOnlyWhatALaterEventRepeats() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  g_rec.calls.clear();
  FeedParty(w, "PartyCreateSuccess", 700, kSelf);   // Created
  FeedParty(w, "PartyJoinNotify", 700, 3001);       // MemberJoined(1)
  FeedParty(w, "PartyJoinFailure", 0, 3);           // JoinFailed(3)
  FeedParty(w, "PartyPassNotify", 700, 3001);       // HostChanged
  for (int i = 0; i < 150; ++i) {                   // 300 events a later one of its kind repeats
    FeedParty(w, "PartyUpdateNotify", 700, 0);       // Updated
    FeedParty(w, "PartyUpdateMemberNotify", 700, 3001);  // MemberUpdated(1)
  }
  FeedParty(w, "PartyJoinFailure", 0, 4);           // JoinFailed(4)
  FeedParty(w, "PartyPassNotify", 700, kSelf);      // HostChanged
  for (int i = 0; i < 10; ++i) Update(w, 0);
  QCHECK(g_rec.calls.size() == 256);
  QCHECK(FacadeCountersView().eventsDropped.load() == 50);  // 306 events, 256 kept
  QCHECK(g_rec.calls.size() == 256 && g_rec.calls[0] == "v" + std::to_string(kCbCreated));
  QCHECK(g_rec.calls.size() == 256 && g_rec.calls[1] == "u" + std::to_string(kCbMemberJoined) + ":1");
  QCHECK(g_rec.calls.size() == 256 && g_rec.calls[2] == "u" + std::to_string(kCbJoinFailed) + ":3");
  QCHECK(g_rec.calls.size() == 256 && g_rec.calls[3] == "v" + std::to_string(kCbHostChanged));
  QCHECK(g_rec.calls.size() == 256 && g_rec.calls[254] == "u" + std::to_string(kCbJoinFailed) + ":4");
  QCHECK(g_rec.calls.size() == 256 && g_rec.calls[255] == "v" + std::to_string(kCbHostChanged));
  QCHECK(CountLines("social_events_dropped") == 1);  // logged once, counted always
}

void TestRepeatedUpdatesAreMerged() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  FeedParty(w, "PartyJoinNotify", 777, 3001);
  Update(w, 0);
  g_rec.calls.clear();
  for (int i = 0; i < 50; ++i) FeedParty(w, "PartyUpdateNotify", 777, 0);          // 50 Updated in a row: one
  for (int i = 0; i < 50; ++i) FeedParty(w, "PartyUpdateMemberNotify", 777, 3001);  // 50 MemberUpdated(1): one
  Update(w, 0);
  QCHECK(CalledCount("v" + std::to_string(kCbUpdated)) == 1);
  QCHECK(CalledCount("u" + std::to_string(kCbMemberUpdated) + ":1") == 1);
}

void TestEventQueueHardLimit() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  // Events that are never dropped for room still stop somewhere: past 4096 the newest is dropped and counted.
  for (int i = 0; i < 5000; ++i) FeedParty(w, "PartyJoinFailure", 0, 3);
  Update(w, 0);
  QCHECK(FacadeCountersView().eventsDropped.load() == 5000 - 4096);
  QCHECK(CountLines("social_events_dropped") == 1);
}

void TestMemberCountNeverExceedsTheGamesArray() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);  // the local user alone: 1 member
  void* obj = w.Obj();
  g_rec.calls.clear();
  for (std::uint64_t id = 3000; id < 3008; ++id) FeedParty(w, "PartyJoinNotify", 777, id);  // 9 members
  Update(w, 0);
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == 9);
  QCHECK(Get32(obj, kOffMemberCount) == 9);
  FeedParty(w, "PartyJoinNotify", 777, 3008);  // 10: exactly the array
  Update(w, 0);
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == kMemberJsonSlots);
  QCHECK(Get32(obj, kOffMemberCount) == kMemberJsonSlots);
  QCHECK(FacadeCountersView().membersHidden.load() == 0);
  // 11 and 12: past the array. The game indexes it with the index each MemberJoined carries
  // (CR15NetGame::PartyMemberJoinedCB, libr15 0x126f8ac) and checks nothing, so no callback may carry 10 or 11.
  FeedParty(w, "PartyJoinNotify", 777, 3009);
  FeedParty(w, "PartyJoinNotify", 777, 3010);
  Update(w, 0);
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == kMemberJsonSlots);
  QCHECK(Get32(obj, kOffMemberCount) == kMemberJsonSlots);
  QCHECK(FacadeCountersView().membersHidden.load() == 2);
  QCHECK(CountLines("social_members_hidden") == 1);  // once per party
  const std::vector<std::size_t> joined = JoinedIndices();
  QCHECK(joined.size() == 9);  // members 1..9; the two past the array were not announced
  for (const std::size_t index : joined) QCHECK(index < kMemberJsonSlots);
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(obj, kMemberJsonSlots - 1) == 3008);
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(obj, kMemberJsonSlots) == 0);  // past the array: answered, never read
  QCHECK(std::string(SlotFn<Str_U32>(obj, kMemberName)(obj, 12)).empty());

  // A hidden member's updates are not delivered either; a visible member's are, at its position.
  g_rec.calls.clear();
  FeedParty(w, "PartyUpdateMemberNotify", 777, 3010);
  FeedParty(w, "PartyUpdateMemberNotify", 777, 3001);
  Update(w, 0);
  QCHECK(g_rec.calls.size() == 1 && Called("u" + std::to_string(kCbMemberUpdated) + ":2"));

  // A visible member leaves: the game hears MemberLeft, and the first hidden member moves into the window and
  // is announced at the position it takes (9), after it.
  g_rec.calls.clear();
  FeedParty(w, "PartyLeaveNotify", 777, 3000);
  Update(w, 0);
  QCHECK(g_rec.calls.size() == 2);
  QCHECK(Called("n" + std::to_string(kCbMemberLeft) + ":3000:3000"));
  QCHECK(Called("u" + std::to_string(kCbMemberJoined) + ":9"));
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == kMemberJsonSlots);
  QCHECK(SlotFn<U64_U32>(obj, kMemberId)(obj, kMemberJsonSlots - 1) == 3009);

  // The member still hidden (3010) leaves: the game never heard of it, so it hears nothing.
  g_rec.calls.clear();
  FeedParty(w, "PartyLeaveNotify", 777, 3010);
  Update(w, 0);
  QCHECK(g_rec.calls.empty());
  QCHECK(SlotFn<U32_0>(obj, kMemberCount)(obj) == kMemberJsonSlots);

  // Eleven members again, then the host passes to the one past the array: the game is told the host changed,
  // Host answers with that id, and no index goes out of range.
  FeedParty(w, "PartyJoinNotify", 777, 3011);
  Update(w, 0);
  g_rec.calls.clear();
  FeedParty(w, "PartyPassNotify", 777, 3011);
  Update(w, 0);
  QCHECK(Called("v" + std::to_string(kCbHostChanged)));
  QCHECK(SlotFn<U64_0>(obj, kHost)(obj) == 3011);
  QCHECK(SlotFn<U32_0>(obj, kIsHost)(obj) == 0);
  QCHECK(Get32(obj, kOffOwnerIndex) < kMemberJsonSlots);
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

// A fake of the game's CJson: sixteen bytes whose first word points at the document text (a heap string; zero is the
// empty document, which is what a zeroed CJson is). Decode replaces the document, Reset frees it, EncodeToCompact
// writes it ("{}" when empty). A text with a "BAD" key does not load (the game refuses it).
std::string* DocOf(const void* cjson) {
  std::string* doc = nullptr;
  std::memcpy(&doc, cjson, sizeof(doc));
  return doc;
}
void SetDoc(void* cjson, const std::string& text) {
  delete DocOf(cjson);
  std::string* doc = new std::string(text);
  std::memcpy(cjson, &doc, sizeof(doc));
}
std::string DocText(const void* cjson) {
  const std::string* doc = DocOf(cjson);
  return doc != nullptr ? *doc : std::string();
}
int g_fakeResets = 0;
int g_fakeDecodes = 0;
void* g_lastResetArg = nullptr;
std::vector<void*> g_resetArgs;
void FakeReset(void* cjson) {
  ++g_fakeResets;
  g_lastResetArg = cjson;
  g_resetArgs.push_back(cjson);
  delete DocOf(cjson);
  std::memset(cjson, 0, 16);
}
unsigned FakeDecode(void* cjson, const char* text, unsigned long long length) {
  ++g_fakeDecodes;
  const std::string t(text, static_cast<std::size_t>(length));
  if (t.find("\"BAD\"") != std::string::npos) return 7;
  SetDoc(cjson, t);
  // The slot's offset in the member array, to see which slot loaded, in order with the callbacks.
  g_rec.calls.push_back("load:" + t);
  return 0;
}
int g_encodeFails = 0;
unsigned FakeEncode(const void* cjson, char* out, unsigned long long* size, unsigned, const char* path) {
  if (g_encodeFails > 0 || path == nullptr || path[0] != '\0') return 9;
  const std::string* doc = DocOf(cjson);
  const std::string text = doc != nullptr ? *doc : std::string("{}");
  if (text.size() > *size) return 1;
  std::memcpy(out, text.data(), text.size());
  *size = text.size();
  return 0;
}
void UseFakeJson() {
  GameJson json;
  json.reset = &FakeReset;
  json.decode = &FakeDecode;
  json.encode = &FakeEncode;
  SetGameJson(json);
  g_fakeResets = 0;
  g_fakeDecodes = 0;
  g_lastResetArg = nullptr;
  g_resetArgs.clear();
  g_encodeFails = 0;
}
std::uint8_t* PartyJson(void* obj) { return static_cast<std::uint8_t*>(obj) + kOffPartyJson; }
std::uint8_t* MemberJson(void* obj, std::size_t slot) {
  std::uintptr_t base = 0;
  std::memcpy(&base, static_cast<std::uint8_t*>(obj) + kOffMemberJson, sizeof(base));
  return reinterpret_cast<std::uint8_t*>(base) + 16 * slot;
}
void FreeDocs(void* obj) {  // a test's own cleanup of the fake documents
  delete DocOf(PartyJson(obj));
  std::memset(PartyJson(obj), 0, 16);
  for (std::size_t i = 0; i < kMemberJsonSlots; ++i) {
    delete DocOf(MemberJson(obj, i));
    std::memset(MemberJson(obj, i), 0, 16);
  }
}

std::string DataNotifyPayload(std::uint64_t party, std::uint64_t member, std::uint32_t seq, const std::string& json) {
  return Le(party, 8) + Le(member, 8) + Le(seq, 4) + Le(json.size(), 4) + json;
}

// CNSISocial::Reset clears the party CJson at +0x1f0 and every member CJson with CJson::Reset (libpnsovr 0x36a92c,
// 0x36a974): the social object owns them, so the old party's lobby settings and the members' data must not outlive the
// party. The facade calls the game's own function on all eleven (it cannot free a tree it did not allocate), from
// social_game_calls.cpp.
void TestResetCallsTheGamesCJsonReset() {
  World w;
  void* obj = w.Obj();
  UseFakeJson();
  std::memset(static_cast<std::uint8_t*>(obj) + kOffLobbyUuid, 0xAB, 16);
  SetDoc(PartyJson(obj), "{\"lobbyid\":\"x\"}");
  SetDoc(MemberJson(obj, 0), "{\"headsettype\":1}");
  SlotFn<Void0>(obj, kReset)(obj);
  QCHECK(g_fakeResets == 1 + static_cast<int>(kMemberJsonSlots));
  QCHECK(g_resetArgs.size() == 1 + kMemberJsonSlots && g_resetArgs[0] == static_cast<void*>(PartyJson(obj)));
  for (std::size_t i = 0; i < kMemberJsonSlots && g_resetArgs.size() == 1 + kMemberJsonSlots; ++i) {
    QCHECK(g_resetArgs[1 + i] == static_cast<void*>(MemberJson(obj, i)));
  }
  QCHECK(DocOf(PartyJson(obj)) == nullptr && DocOf(MemberJson(obj, 0)) == nullptr);  // freed by the game's function
  const std::uint8_t zero[16] = {};
  QCHECK(std::memcmp(static_cast<std::uint8_t*>(obj) + kOffLobbyUuid, zero, 16) == 0);  // Reset stores kInvalid (zero)
  QCHECK(FacadeCountersView().jsonFailed.load() == 0);
  SlotFn<Void0>(obj, kReset)(obj);
  QCHECK(g_fakeResets == 2 * (1 + static_cast<int>(kMemberJsonSlots)));

  // The game's function is not known (libr15 absent or not the pinned build): the CJson are left alone, not
  // zeroed (that would leak the tree the game built), and the Reset is counted.
  SetGameJson(GameJson{});
  std::uint8_t pattern[16];
  for (int i = 0; i < 16; ++i) pattern[i] = static_cast<std::uint8_t>(0xA0 + i);
  std::memcpy(PartyJson(obj), pattern, 16);
  const int before = g_fakeResets;
  SlotFn<Void0>(obj, kReset)(obj);
  QCHECK(g_fakeResets == before);
  QCHECK(std::memcmp(PartyJson(obj), pattern, 16) == 0);
  QCHECK(FacadeCountersView().jsonFailed.load() == 1);
  std::memset(PartyJson(obj), 0, 16);
}

// The server's party and member data reaches the game's JSON, as on the PC: loaded before the callbacks fire (a
// MemberJoined callback already finds the member's headsettype), then MemberUpdated for a member whose data changed
// and Updated for the party's.
void TestReceivedMemberDataIsLoadedBeforeTheCallbacks() {
  World w;
  UseFakeJson();
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  void* obj = w.Obj();
  g_rec.calls.clear();
  // The data of a member the party has not heard of yet adds the member (MemberJoined), with its data.
  Feed(w, SocialParty::kPartyDataNotify, DataNotifyPayload(777, 3001, 1, "{\"headsettype\":3}"));
  Update(w, 0);
  const std::string joined = "u" + std::to_string(kCbMemberJoined) + ":1";
  const std::string updated = "u" + std::to_string(kCbMemberUpdated) + ":1";
  QCHECK(g_rec.calls.size() == 3);
  QCHECK(g_rec.calls.size() == 3 && g_rec.calls[0] == "load:{\"headsettype\":3}");  // loaded first
  QCHECK(g_rec.calls.size() == 3 && g_rec.calls[1] == joined);
  QCHECK(g_rec.calls.size() == 3 && g_rec.calls[2] == updated);
  QCHECK(DocText(MemberJson(obj, 1)) == "{\"headsettype\":3}");  // the game reads member 1's data at +0x248 + 16
  QCHECK(DocOf(MemberJson(obj, 2)) == nullptr);

  // New data for the same member replaces it; the same data again changes nothing.
  g_rec.calls.clear();
  Feed(w, SocialParty::kPartyDataNotify, DataNotifyPayload(777, 3001, 2, "{\"headsettype\":4}"));
  Update(w, 0);
  QCHECK(DocText(MemberJson(obj, 1)) == "{\"headsettype\":4}");
  QCHECK(CalledCount(updated) == 1);
  g_rec.calls.clear();
  Update(w, 0);
  QCHECK(g_rec.calls.empty());

  // The member leaves: its slot is cleared, and the member after it moves up with its data.
  FeedParty(w, "PartyJoinNotify", 777, 3002);
  Feed(w, SocialParty::kPartyDataNotify, DataNotifyPayload(777, 3002, 1, "{\"headsettype\":5}"));
  Update(w, 0);
  QCHECK(DocText(MemberJson(obj, 2)) == "{\"headsettype\":5}");
  FeedParty(w, "PartyLeaveNotify", 777, 3001);
  g_fakeResets = 0;
  Update(w, 0);
  QCHECK(DocText(MemberJson(obj, 1)) == "{\"headsettype\":5}");
  QCHECK(DocOf(MemberJson(obj, 2)) == nullptr);
  QCHECK(g_fakeResets == 1);
  QCHECK(FacadeCountersView().jsonFailed.load() == 0);
  FreeDocs(obj);
}

void TestPartyDataIsLoadedForAMemberNotForTheLeader() {
  World w;
  UseFakeJson();
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  // Joined as a member of 2002's party: its party data goes to +0x1f0 and the game is told the party updated.
  FeedParty(w, "PartyJoinSuccess", 556, 2002);
  Update(w, 0);
  g_rec.calls.clear();
  Feed(w, SocialParty::kPartyDataNotify, DataNotifyPayload(556, 0, 1, "{\"lobbyid\":\"abc\"}"));
  Update(w, 0);
  QCHECK(DocText(PartyJson(obj)) == "{\"lobbyid\":\"abc\"}");
  QCHECK(CalledCount("v" + std::to_string(kCbUpdated)) == 1);
  QCHECK(Called("load:{\"lobbyid\":\"abc\"}"));

  // As the leader, the server's party data is the leader's own: ignored, and counted as an ignored frame.
  World l;
  UseFakeJson();
  Init(l, MakeCallbacks());
  CreateParty(l, 777);
  g_lines.clear();
  Feed(l, SocialParty::kPartyDataNotify, DataNotifyPayload(777, 0, 1, "{\"lobbyid\":\"mine\"}"));
  Update(l, 0);
  QCHECK(DocOf(PartyJson(l.Obj())) == nullptr);
  QCHECK(FacadeCountersView().framesIgnored.load() == 1);
  QCHECK(CountLines("\"event\":\"social_frame_ignored\"") == 1 && CountLines("\"why\":\"party_data_own\"") == 1);
  FreeDocs(obj);
  FreeDocs(l.Obj());
}

void TestUnreadablePartyDataIsIgnoredAndSaysWhy() {
  World w;
  UseFakeJson();
  CreateParty(w, 777);
  g_lines.clear();
  Feed(w, SocialParty::kPartyDataNotify, DataNotifyPayload(777, 3001, 1, "[1,2]"));  // not an object
  Feed(w, SocialParty::kPartyDataNotify, Le(777, 8));                                // shorter than its header
  Feed(w, SocialParty::kPartyDataNotify, DataNotifyPayload(999, 3001, 1, "{\"a\":1}"));  // another party
  QCHECK(FacadeCountersView().framesIgnored.load() == 3);
  QCHECK(CountLines("\"why\":\"party_data_not_a_json_object\"") == 1);
  QCHECK(CountLines("\"why\":\"party_data_unreadable\"") == 1);
  QCHECK(CountLines("\"why\":\"party_data_other_party\"") == 1);
  Update(w, 0);
  QCHECK(g_fakeDecodes == 0);
}

// Without the game's functions the data is held, counted once and loaded when they become known.
void TestDataWaitsForTheGamesFunctions() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  void* obj = w.Obj();
  FeedParty(w, "PartyJoinNotify", 777, 3001);
  Feed(w, SocialParty::kPartyDataNotify, DataNotifyPayload(777, 3001, 1, "{\"headsettype\":3}"));
  g_lines.clear();
  Update(w, 0);
  Update(w, 0);
  QCHECK(FacadeCountersView().jsonFailed.load() == 1);  // counted once, not every frame
  QCHECK(CountLines("\"result\":\"game_json_unavailable\"") == 1);
  UseFakeJson();
  Update(w, 0);
  QCHECK(DocText(MemberJson(obj, 1)) == "{\"headsettype\":3}");
  FreeDocs(obj);
}

void TestARejectedLoadIsCounted() {
  World w;
  UseFakeJson();
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  FeedParty(w, "PartyJoinNotify", 777, 3001);
  Feed(w, SocialParty::kPartyDataNotify, DataNotifyPayload(777, 3001, 1, "{\"BAD\":1}"));
  g_lines.clear();
  g_rec.calls.clear();
  Update(w, 0);
  QCHECK(g_fakeDecodes == 1);
  QCHECK(FacadeCountersView().jsonFailed.load() == 1);
  QCHECK(CountLines("\"result\":\"load_failed\"") == 1);
  QCHECK(!Called("u" + std::to_string(kCbMemberUpdated) + ":1"));  // no MemberUpdated for data the game refused
}

// What the game writes into the party and member CJson goes to the server: the leader's party data when the game marked
// it written (flag bit 0), the local member's after MemberDataWritable handed it out; both once on entering a party.
void TestWrittenDataIsSharedWithTheServer() {
  World w;
  UseFakeJson();
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  QCHECK(SlotFn<U64_U32>(obj, kMemberDataWritable)(obj, 0) == 0);  // no local user yet
  SlotFn<Void_U32>(obj, kAddMember)(obj, 0);
  QCHECK(SlotFn<U64_U32>(obj, kMemberDataWritable)(obj, 1) == 0);  // only the local member's is writable
  QCHECK(SlotFn<U64_U32>(obj, kMemberDataWritable)(obj, 0) == reinterpret_cast<std::uint64_t>(MemberJson(obj, 0)));
  Update(w, 1);
  FeedParty(w, "PartyCreateSuccess", 777, kSelf);
  g_sent.clear();
  Update(w, 0);  // entering the party shares both (the game's documents are empty: "{}")
  const auto shared = [&](std::uint64_t scope) {
    std::vector<std::string> texts;
    for (const SocialParty::Message& m : g_sent) {
      if (m.symbol != SocialParty::kPartyDataUpdateRequest) continue;
      if (PayloadU64(m.payload, 0x20) != scope) continue;
      texts.push_back(m.payload.substr(0x28 + 8));  // seq(4) length(4) then the text
    }
    return texts;
  };
  QCHECK(shared(SocialParty::kPartyDataScopeParty) == std::vector<std::string>{"{}"});
  QCHECK(shared(SocialParty::kPartyDataScopeMember) == std::vector<std::string>{"{}"});

  // The game writes the party data and marks it (bit 0): shared once, the bit cleared.
  SetDoc(PartyJson(obj), "{\"lobbyid\":\"abc\"}");
  const std::uint32_t flags = Get32(obj, kOffFlags) | kFlagDataWritten;
  std::memcpy(static_cast<std::uint8_t*>(obj) + kOffFlags, &flags, sizeof(flags));
  g_sent.clear();
  Update(w, 0);
  QCHECK(shared(SocialParty::kPartyDataScopeParty) == std::vector<std::string>{"{\"lobbyid\":\"abc\"}"});
  QCHECK(shared(SocialParty::kPartyDataScopeMember).empty());
  QCHECK((Get32(obj, kOffFlags) & kFlagDataWritten) == 0);
  g_sent.clear();
  Update(w, 0);
  QCHECK(shared(SocialParty::kPartyDataScopeParty).empty());  // not again until the game marks it

  // The game takes the member's JSON (slot 31) and writes the headset type: shared as the member's.
  SetDoc(MemberJson(obj, 0), "{\"headsettype\":2}");
  SlotFn<U64_U32>(obj, kMemberDataWritable)(obj, 0);
  g_sent.clear();
  Update(w, 0);
  QCHECK(shared(SocialParty::kPartyDataScopeMember) == std::vector<std::string>{"{\"headsettype\":2}"});
  QCHECK(shared(SocialParty::kPartyDataScopeParty).empty());

  // A game JSON that cannot be read out is counted, and nothing is sent.
  SlotFn<U64_U32>(obj, kMemberDataWritable)(obj, 0);
  g_encodeFails = 1;
  g_sent.clear();
  Update(w, 0);
  QCHECK(shared(SocialParty::kPartyDataScopeMember).empty());
  QCHECK(FacadeCountersView().jsonFailed.load() == 1);
  FreeDocs(obj);
}

void TestAMemberDoesNotShareThePartyData() {
  World w;
  UseFakeJson();
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  FeedParty(w, "PartyJoinSuccess", 556, 2002);
  Update(w, 0);
  g_sent.clear();
  SetDoc(PartyJson(obj), "{\"x\":1}");
  const std::uint32_t flags = Get32(obj, kOffFlags) | kFlagDataWritten;
  std::memcpy(static_cast<std::uint8_t*>(obj) + kOffFlags, &flags, sizeof(flags));
  Update(w, 0);
  for (const SocialParty::Message& m : g_sent) {
    QCHECK(!(m.symbol == SocialParty::kPartyDataUpdateRequest && PayloadU64(m.payload, 0x20) == SocialParty::kPartyDataScopeParty));
  }
  FreeDocs(obj);
}

// A lock the sender refuses is asked again, but not every frame: the game calls Update once per frame and the
// host's joinable bit disagrees with the server's lock until a request gets through.
void TestRefusedLockIsRetriedOnABackoff() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  void* obj = w.Obj();
  const std::uint32_t flags = Get32(obj, kOffFlags);
  const std::uint32_t locked = flags & ~kFlagJoinable;  // the host wants the party closed; the server has it open
  std::memcpy(static_cast<std::uint8_t*>(obj) + kOffFlags, &locked, sizeof(locked));
  g_sendOk = false;
  g_sent.clear();
  g_lines.clear();
  for (int i = 0; i < 100; ++i) Update(w, 0);  // one second, a hundred frames
  QCHECK(SentCount(SocialParty::kLockRequest) == 1);
  QCHECK(CountLines("\"name\":\"PartyLockRequest\"") == 1);
  QCHECK(FacadeCountersView().sendFailed.load() == 1);
  for (int i = 0; i < 100; ++i) {  // a hundred seconds, a frame each
    g_now += 1;
    Update(w, 0);
  }
  const std::size_t attempts = SentCount(SocialParty::kLockRequest);
  QCHECK(attempts >= 19 && attempts <= 21);  // one per 5 s
  QCHECK(CountLines("\"name\":\"PartyLockRequest\"") == attempts);  // one log line per attempt
  // The sender recovers: the next attempt after the interval goes through and is not repeated.
  g_sendOk = true;
  g_now += 5;
  g_sent.clear();
  Update(w, 0);
  QCHECK(SentCount(SocialParty::kLockRequest) == 1);
  Update(w, 0);
  QCHECK(SentCount(SocialParty::kLockRequest) == 1);
}

// A request the sender took and the server never answers is failed after 10 s like a refused one. Frame
// times wander; the deadline is measured from the send.
void TestUnansweredCreateTimesOut() {
  World w;
  w.party.SetSelf(kSelf, "alice");
  Update(w, 1);  // t = 1000: the create is sent and taken
  QCHECK(SentCount(SocialParty::kCreateRequest) == 1 && w.party.Snapshot().creating);
  g_now += 9;
  Update(w, 1);
  QCHECK(w.party.Snapshot().creating && FacadeCountersView().requestTimeout.load() == 0);  // not yet
  g_now += 1;
  Update(w, 1);  // t = 1010
  QCHECK(FacadeCountersView().requestTimeout.load() == 1);
  QCHECK(CountLines("social_request_timeout") == 1);
  QCHECK(SentCount(SocialParty::kCreateRequest) == 2);  // rolled back, and the game still wants a party: asked again
  // 600 more seconds of silence, jittered frame times: one attempt per 10 s at most, never faster.
  for (std::size_t i = 0; i < 400 && g_now < 1610; ++i) {
    g_now += Jitter(i);
    Update(w, 1);
  }
  const std::size_t creates = SentCount(SocialParty::kCreateRequest);
  QCHECK(creates >= 40 && creates <= 62);
  for (std::size_t i = 1; i < g_sentAt.size(); ++i) QCHECK(g_sentAt[i] - g_sentAt[i - 1] >= 10);
  QCHECK(FacadeCountersView().requestTimeout.load() + 1 >= creates && FacadeCountersView().requestTimeout.load() <= creates);
  // A late reply is applied as usual.
  FeedParty(w, "PartyCreateSuccess", 777, kSelf);
  Update(w, 0);
  QCHECK(w.party.Snapshot().partyId == 777 && !w.party.Snapshot().creating);
}

void TestAnsweredCreateAndLockDoNotTimeOut() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);  // sent and answered within the frame
  void* obj = w.Obj();
  const std::uint32_t closed = Get32(obj, kOffFlags) & ~kFlagJoinable;
  std::memcpy(static_cast<std::uint8_t*>(obj) + kOffFlags, &closed, sizeof(closed));
  Update(w, 0);  // the lock request goes out
  QCHECK(SentCount(SocialParty::kLockRequest) == 1);
  FeedParty(w, "PartyLockSuccess", 777, 0);  // and is answered
  for (std::size_t i = 0; i < 100; ++i) {
    g_now += Jitter(i);
    Update(w, 1);
  }
  QCHECK(FacadeCountersView().requestTimeout.load() == 0);
  QCHECK(SentCount(SocialParty::kLockRequest) == 1 && SentCount(SocialParty::kCreateRequest) == 1);
}

void TestUnansweredLockTimesOut() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  void* obj = w.Obj();
  const std::uint32_t closed = Get32(obj, kOffFlags) & ~kFlagJoinable;
  std::memcpy(static_cast<std::uint8_t*>(obj) + kOffFlags, &closed, sizeof(closed));
  g_sent.clear();
  g_sentAt.clear();
  const std::uint64_t start = g_now;
  Update(w, 0);  // taken, never answered
  QCHECK(SentCount(SocialParty::kLockRequest) == 1);
  for (std::size_t i = 0; i < 400 && g_now < start + 600; ++i) {
    g_now += Jitter(i);
    Update(w, 0);
  }
  const std::size_t attempts = SentCount(SocialParty::kLockRequest);
  QCHECK(attempts >= 40 && attempts <= 62);
  for (std::size_t i = 1; i < g_sentAt.size(); ++i) QCHECK(g_sentAt[i] - g_sentAt[i - 1] >= 10);
  QCHECK(FacadeCountersView().requestTimeout.load() + 1 >= attempts);
  QCHECK(CountLines("\"name\":\"PartyLockRequest\"") == attempts);
}

// The probe that found it: a create the sender took and the server never answered stuck the model for good, and
// every join after it was deferred. Now the create times out, the deferred join goes ahead, and a join the
// server never answers (a locked party queues it without a reply) fails to the game.
void TestUnansweredJoinFailsToTheGame() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  Update(w, 1);  // a create, taken and never answered
  SlotFn<Void_U64>(w.Obj(), kJoinInternal)(w.Obj(), 556);  // deferred behind it
  QCHECK(SentCount(SocialParty::kJoinRequest) == 0);
  g_rec.calls.clear();
  for (std::size_t i = 0; i < 100 && SentCount(SocialParty::kJoinRequest) == 0; ++i) {
    g_now += Jitter(i);
    Update(w, 1);
  }
  QCHECK(SentCount(SocialParty::kJoinRequest) == 1);  // went out once the create was given up
  const std::uint64_t joinSentAt = g_sentAt[g_sentAt.size() - 1];
  QCHECK(w.party.Snapshot().joining);
  // 600 s of silence: the join fails once, to the game, and is not sent again by the facade.
  for (std::size_t i = 0; i < 400 && g_now < joinSentAt + 600; ++i) {
    g_now += Jitter(i);
    Update(w, 0);
  }
  QCHECK(!w.party.Snapshot().joining);
  QCHECK(CalledCount("u" + std::to_string(kCbJoinFailed) + ":0") == 1);
  QCHECK((Get32(w.Obj(), kOffFlags) & kFlagJoining) == 0);
  QCHECK(SentCount(SocialParty::kJoinRequest) == 1);
  QCHECK(FacadeCountersView().requestTimeout.load() == 2);  // the create and the join
}

// A join the sender refuses: the invite it consumed comes back, the request type of a retry is the invite's
// accept again, and the game is told (JoinFailed, code 0).
void TestRefusedInviteJoinKeepsTheInviteAndTellsTheGame() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  FeedParty(w, "PartyInviteNotify", 556, 2002);
  Update(w, 0);
  QCHECK(SlotFn<U32_0>(obj, kInviteCount)(obj) == 1);
  g_rec.calls.clear();
  g_rec.gate = 1;
  g_sendOk = false;
  g_sent.clear();
  SlotFn<Void_U32>(obj, kAcceptInvite)(obj, 0);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kInviteResponse);
  QCHECK(!w.party.Snapshot().joining);
  QCHECK(w.party.Snapshot().invites.size() == 1);  // given back
  Update(w, 0);
  QCHECK(CalledCount("u" + std::to_string(kCbJoinFailed) + ":0") == 1);  // the game was told
  QCHECK(FacadeCountersView().cbJoinFailed.load() == 1);
  QCHECK(SlotFn<U32_0>(obj, kInviteCount)(obj) == 1);
  // The retry (the same accept, or a join by party id) is the invite's accept to the inviter again.
  g_sendOk = true;
  g_sent.clear();
  SlotFn<Void_U64>(obj, kJoinInternal)(obj, 556);
  QCHECK(g_sent.size() == 1 && g_sent[0].symbol == SocialParty::kInviteResponse);
  QCHECK(g_sent.size() == 1 && g_sent[0].target == 2002);
  QCHECK(w.party.Snapshot().joining);
}

void TestRequestLogsCarryTheTargetAccount() {
  World w;
  Init(w, MakeCallbacks());
  CreateParty(w, 777);
  void* obj = w.Obj();
  FeedParty(w, "PartyJoinNotify", 777, 3003);
  FeedParty(w, "PartyJoinNotify", 777, 4004);
  Update(w, 0);
  g_lines.clear();
  SlotFn<Void_U32>(obj, kKick)(obj, 1);  // 3003
  QCHECK(CountLines("\"name\":\"PartyKickRequest\"") == 1);
  QCHECK(CountLines("\"target\":3003") == 1);
  g_lines.clear();
  Update(w, 0);
  SlotFn<Void_U32>(obj, kPassOwnership)(obj, 1);  // 4004 is now at 1
  QCHECK(CountLines("\"name\":\"PartyPassRequest\"") == 1);
  QCHECK(CountLines("\"target\":4004") == 1);
  g_lines.clear();
  FeedParty(w, "PartyInviteNotify", 600, 7007);
  Update(w, 0);
  SlotFn<Void_U32>(obj, kDismissInvite)(obj, 0);
  QCHECK(CountLines("\"name\":\"PartyInviteResponse\"") == 1);
  QCHECK(CountLines("\"target\":7007") == 1);
  QCHECK(CountLines("\"param\":0") >= 1);
  g_lines.clear();
  FeedParty(w, "PartyInviteNotify", 601, 8008);
  Update(w, 0);
  g_rec.gate = 1;
  SlotFn<Void_U32>(obj, kAcceptInvite)(obj, 0);
  QCHECK(CountLines("\"target\":8008") == 1);
  QCHECK(CountLines("\"param\":1") == 1);
  QCHECK(CountLines("\"party\":") >= 1);
  g_lines.clear();
  SlotFn<Void0>(obj, kRefreshRecentlyMetUsers)(obj);
  QCHECK(CountLines("\"name\":\"RecentlyMetRefreshRequest\"") == 1);
}

// A refused invite stays queued behind its create; asking again must not queue it twice.
void TestInviteIsQueuedOnce() {
  World w;
  Init(w, MakeCallbacks());
  w.party.SetSelf(kSelf, "alice");
  void* obj = w.Obj();
  g_sendOk = false;
  SlotFn<Void_U64>(obj, kSendInviteInternal)(obj, 2002);  // the create is refused; the invite waits
  SlotFn<Void_U64>(obj, kSendInviteInternal)(obj, 2002);  // the user asks again
  g_sendOk = true;
  SlotFn<Void_U64>(obj, kSendInviteInternal)(obj, 2002);  // and again, this time the create goes
  g_sent.clear();
  FeedParty(w, "PartyCreateSuccess", 777, kSelf);
  std::size_t invites = 0;
  for (const SocialParty::Message& m : g_sent) invites += m.symbol == SocialParty::kInviteRequest ? 1 : 0;
  QCHECK(invites == 1);
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
  QCHECK(CountLines("\"name\":\"OtherUserProfileRequest\"") == 1);  // the log names the account asked about
  QCHECK(CountLines("\"target\":2002") >= 1);
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
  TestInviteNotificationsAreMerged();
  TestEventQueueDropsOnlyWhatALaterEventRepeats();
  TestRepeatedUpdatesAreMerged();
  TestEventQueueHardLimit();
  TestMemberCountNeverExceedsTheGamesArray();
  TestFailedSendsDoNotStickTheModel();
  TestDeferredJoinLogsOncePerParty();
  TestResetCallsTheGamesCJsonReset();
  TestReceivedMemberDataIsLoadedBeforeTheCallbacks();
  TestPartyDataIsLoadedForAMemberNotForTheLeader();
  TestUnreadablePartyDataIsIgnoredAndSaysWhy();
  TestDataWaitsForTheGamesFunctions();
  TestARejectedLoadIsCounted();
  TestWrittenDataIsSharedWithTheServer();
  TestAMemberDoesNotShareThePartyData();
  TestRefusedLockIsRetriedOnABackoff();
  TestUnansweredCreateTimesOut();
  TestAnsweredCreateAndLockDoNotTimeOut();
  TestUnansweredLockTimesOut();
  TestUnansweredJoinFailsToTheGame();
  TestRefusedInviteJoinKeepsTheInviteAndTellsTheGame();
  TestRequestLogsCarryTheTargetAccount();
  TestInviteIsQueuedOnce();
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
