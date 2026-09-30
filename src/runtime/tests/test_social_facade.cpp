#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "runtime/compat/social_names.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"
#include "runtime/patch/party_invite_gate.h"
#include "runtime/patch/social_facade.h"
#include "core/hooking.h"

namespace {

using Slot = std::uintptr_t;

const Slot* Vtable(void* object) { return *static_cast<const Slot**>(object); }

TEST(SocialFacade, FlagOffRequestsOnlyTheAccessorHook) {
  std::vector<SocialFacade::Probe> requests;
  const auto fakeInstall = [&requests](SocialFacade::Probe probe) { requests.push_back(probe); };
  SocialFacade::InstallHookPlan(SocialFacade::InstallStage::kBoot, false, fakeInstall);
  SocialFacade::InstallHookPlan(SocialFacade::InstallStage::kFacadeSelected, false, fakeInstall);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0], SocialFacade::Probe::kAccessor);
  EXPECT_EQ(SocialFacade::Select(false, nullptr), nullptr);

  int providerObject = 0;
  EXPECT_EQ(SocialFacade::Select(false, &providerObject), &providerObject);
  EXPECT_EQ(SocialFacade::Select(true, &providerObject), &providerObject);
}

TEST(SocialFacade, EnabledInstallPublishesEachTrampolineBeforeEnable) {
  std::vector<SocialFacade::Probe> requests;
  const auto fakeInstall = [&requests](SocialFacade::Probe probe) { requests.push_back(probe); };
  SocialFacade::InstallHookPlan(SocialFacade::InstallStage::kBoot, true, fakeInstall);
  SocialFacade::InstallHookPlan(SocialFacade::InstallStage::kFacadeSelected, true, fakeInstall);
  ASSERT_EQ(requests.size(), 4u);
  EXPECT_EQ(requests[0], SocialFacade::Probe::kAccessor);
  EXPECT_EQ(requests[1], SocialFacade::Probe::kSocialJson);
  EXPECT_EQ(requests[2], SocialFacade::Probe::kJsonSet);
  EXPECT_EQ(requests[3], SocialFacade::Probe::kJsonNavigateForWrite);

  for (std::size_t i = 1; i < requests.size(); ++i) {
    std::vector<std::string> calls;
    void* published = nullptr;
    const auto create = [&](void** trampoline) {
      calls.emplace_back("create");
      *trampoline = reinterpret_cast<void*>(static_cast<std::uintptr_t>(i + 1));
      return true;
    };
    const auto publish = [&](void* trampoline) {
      calls.emplace_back("publish");
      published = trampoline;
    };
    const auto enable = [&] {
      calls.emplace_back("enable");
      return published != nullptr;
    };
    EXPECT_TRUE(Hooking::CreatePublishEnable(create, publish, enable));
    EXPECT_EQ(calls, (std::vector<std::string>{"create", "publish", "enable"}));
  }

  int publishCalls = 0;
  int enableCalls = 0;
  EXPECT_FALSE(Hooking::CreatePublishEnable(
      [](void**) { return false; },
      [&](void*) { ++publishCalls; },
      [&] { ++enableCalls; return true; }));
  EXPECT_EQ(publishCalls, 0);
  EXPECT_EQ(enableCalls, 0);
}

TEST(SocialFacade, JsonTraceRoundTripDrainsOnlyOnce) {
  std::vector<SocialFacade::JsonTraceRecord> records;
  const auto collect = [](const SocialFacade::JsonTraceRecord& record, void* context) {
    static_cast<std::vector<SocialFacade::JsonTraceRecord>*>(context)->push_back(record);
  };
  SocialFacade::QueueJsonTrace(SocialFacade::JsonTraceKind::kSet, 19, "mm|status", 1, 2, 3, 4);
  SocialFacade::DrainJsonTraces(collect, &records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].kind, SocialFacade::JsonTraceKind::kSet);
  EXPECT_EQ(records[0].callCount, 19u);
  EXPECT_STREQ(records[0].path, "mm|status");
  EXPECT_EQ(records[0].argument, 1u);
  EXPECT_EQ(records[0].result, 2u);
  EXPECT_EQ(records[0].root, 3u);
  EXPECT_EQ(records[0].cache, 4u);
  SocialFacade::DrainJsonTraces(collect, &records);
  EXPECT_EQ(records.size(), 1u);

  const char* longPath = "01234567890123456789012345678901234567890123456789";
  SocialFacade::QueueJsonTrace(SocialFacade::JsonTraceKind::kNavigateForWrite, 20, longPath, 5, 6, 7, 8);
  SocialFacade::DrainJsonTraces(collect, &records);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(std::strlen(records[1].path), 47u);
  EXPECT_EQ(records[1].path[47], '\0');
}

TEST(SocialFacade, HasCompleteProcessLifetimeVtable) {
  static_assert(SocialFacade::kRealVtableSlotCount == 75);
  static_assert(SocialFacade::kMaxObservedGameVtableSlot == 76);
  static_assert(SocialFacade::kVtableSlotCount == 85);
  void* first = SocialFacade::Object();
  void* second = SocialFacade::Object();
  ASSERT_EQ(first, second);
  ASSERT_NE(Vtable(first), nullptr);
  for (std::size_t i = 0; i < SocialFacade::kVtableSlotCount; ++i) {
    EXPECT_NE(Vtable(first)[i], 0u) << "slot " << i;
  }
}

TEST(SocialFacade, ProcessLifetimeObjectIsSafeToConstructConcurrently) {
  std::array<void*, 8> objects{};
  std::array<std::thread, 8> threads;
  for (std::size_t i = 0; i < threads.size(); ++i) {
    threads[i] = std::thread([&objects, i] { objects[i] = SocialFacade::Object(); });
  }
  for (auto& thread : threads) thread.join();
  for (void* object : objects) EXPECT_EQ(object, objects[0]);
}

TEST(SocialFacade, InitializeCopiesCallbacksAndRecordsArguments) {
  void* object = SocialFacade::Object();
  std::array<std::uint8_t, 0x1E0> callbacks{};
  for (std::size_t i = 0; i < callbacks.size(); ++i) callbacks[i] = static_cast<std::uint8_t>(i);
  using InitializeFn = std::uint64_t (*)(void*, std::uint32_t, const void*);
  const auto initialize = reinterpret_cast<InitializeFn>(Vtable(object)[9]);
  EXPECT_EQ(initialize(object, 16, callbacks.data()), 0u);
  EXPECT_EQ(std::memcmp(static_cast<std::uint8_t*>(object) + 8, callbacks.data(), callbacks.size()), 0);
  EXPECT_EQ(SocialFacade::TestMaxUsers(), 16u);
  const auto* bytes = static_cast<const std::uint8_t*>(object);
  std::uint64_t arrayPointer = UINT64_MAX;
  std::uint64_t arrayCount = UINT64_MAX;
  std::uint64_t arrayAllocator = UINT64_MAX;
  std::memcpy(&arrayPointer, bytes + 0x248, sizeof(arrayPointer));
  std::memcpy(&arrayCount, bytes + 0x250, sizeof(arrayCount));
  std::memcpy(&arrayAllocator, bytes + 0x258, sizeof(arrayAllocator));
  EXPECT_EQ(arrayPointer, 0u);
  EXPECT_EQ(arrayCount, 0u);
  EXPECT_EQ(arrayAllocator, 0u);
  EXPECT_EQ(SocialFacade::TestCallbacksSource(), callbacks.data());
  EXPECT_GE(SocialFacade::TestInitializeCallCount(), 1u);
}

TEST(SocialFacade, EmptyQueriesReturnSafeDefaults) {
  void* object = SocialFacade::Object();
  using CountFn = std::uint64_t (*)(void*);
  using NameFn = const char* (*)(void*, std::uint32_t);
  using IdFn = std::uint64_t* (*)(void*, std::uint64_t*, std::uint32_t);
  EXPECT_EQ(reinterpret_cast<CountFn>(Vtable(object)[46])(object), 0u);
  EXPECT_STREQ(reinterpret_cast<NameFn>(Vtable(object)[50])(object, 0), "");
  std::uint64_t id = UINT64_MAX;
  EXPECT_EQ(reinterpret_cast<IdFn>(Vtable(object)[49])(object, &id, 0), &id);
  EXPECT_EQ(id, 0u);
}

TEST(SocialFacade, EmptyPartyQueriesUseTheRealSlotContracts) {
  void* object = SocialFacade::Object();
  using UpdateFn = void (*)(void*, const void*);
  using SetLocalUserFn = void (*)(void*, std::uint32_t);
  using QueryFn = std::uint32_t (*)(void*);
  using HostFn = std::uint64_t* (*)(void*, std::uint64_t*);
  using IdFn = std::uint64_t (*)(void*);

  reinterpret_cast<UpdateFn>(Vtable(object)[13])(object, nullptr);
  reinterpret_cast<SetLocalUserFn>(Vtable(object)[14])(object, 0);
  EXPECT_EQ(reinterpret_cast<QueryFn>(Vtable(object)[20])(object), 0u);
  EXPECT_EQ(reinterpret_cast<QueryFn>(Vtable(object)[21])(object), 3u);
  EXPECT_EQ(reinterpret_cast<QueryFn>(Vtable(object)[22])(object), 0u);

  std::uint64_t host = UINT64_MAX;
  EXPECT_EQ(reinterpret_cast<HostFn>(Vtable(object)[23])(object, &host), &host);
  EXPECT_EQ(host, 0u);
  EXPECT_EQ(reinterpret_cast<QueryFn>(Vtable(object)[24])(object), 1u);
  EXPECT_EQ(reinterpret_cast<IdFn>(Vtable(object)[25])(object), 0u);
}

TEST(SocialFacade, ConstructorDefaultsAndPaddedSlotAreSafe) {
  void* object = SocialFacade::Object();
  const auto* bytes = static_cast<const std::uint8_t*>(object);
  std::uint64_t jsonRoot = UINT64_MAX;
  std::uint64_t jsonCache = UINT64_MAX;
  std::uint32_t flags = 0;
  std::uint32_t joinPolicy = 0;
  std::memcpy(&jsonRoot, bytes + 0x1F0, sizeof(jsonRoot));
  std::memcpy(&jsonCache, bytes + 0x1F8, sizeof(jsonCache));
  std::memcpy(&flags, bytes + 0x27C, sizeof(flags));
  std::memcpy(&joinPolicy, bytes + 0x2B4, sizeof(joinPolicy));
  EXPECT_EQ(jsonRoot, 0u);
  EXPECT_EQ(jsonCache, 0u);
  EXPECT_EQ(flags, 2u);
  EXPECT_EQ(joinPolicy, 3u);

  using PaddedFn = std::uint64_t (*)(void*);
  EXPECT_EQ(reinterpret_cast<PaddedFn>(Vtable(object)[76])(object), 0u);
}

TEST(SocialFacade, ShutdownKeepsObjectAndVtableAlive) {
  void* object = SocialFacade::Object();
  const Slot* before = Vtable(object);
  using ShutdownFn = void (*)(void*);
  reinterpret_cast<ShutdownFn>(before[10])(object);
  EXPECT_EQ(SocialFacade::Object(), object);
  EXPECT_EQ(Vtable(object), before);
  EXPECT_GE(SocialFacade::TestShutdownCallCount(), 1u);
}


std::array<std::uint8_t, 24> StatusNotifyPayload(std::uint64_t id, std::uint8_t status) {
  std::array<std::uint8_t, 24> payload{};
  for (int i = 0; i < 8; ++i) payload[8 + i] = static_cast<std::uint8_t>(id >> (8 * i));
  payload[16] = status;
  return payload;
}

std::array<std::uint8_t, 32> ListResponsePayload(std::uint32_t offline, std::uint32_t busy, std::uint32_t online) {
  std::array<std::uint8_t, 32> payload{};
  const std::uint32_t words[3] = {offline, busy, online};
  for (int w = 0; w < 3; ++w)
    for (int i = 0; i < 4; ++i) payload[8 + w * 4 + i] = static_cast<std::uint8_t>(words[w] >> (8 * i));
  return payload;
}

TEST(SocialRoster, ParsesTheTwoFriendMessages) {
  const auto notify = StatusNotifyPayload(0x1122334455667788ULL, SocialRoster::kStatusOffline);
  std::uint64_t id = 0;
  std::uint8_t status = 0;
  ASSERT_TRUE(SocialRoster::ParseStatusNotify(notify.data(), notify.size(), &id, &status));
  EXPECT_EQ(id, 0x1122334455667788ULL);
  EXPECT_EQ(status, SocialRoster::kStatusOffline);
  EXPECT_FALSE(SocialRoster::ParseStatusNotify(notify.data(), 16, &id, &status));

  const auto list = ListResponsePayload(1, 2, 3);
  std::uint32_t confirmed = 0;
  ASSERT_TRUE(SocialRoster::ParseListResponse(list.data(), list.size(), &confirmed));
  EXPECT_EQ(confirmed, 6u);
  EXPECT_FALSE(SocialRoster::ParseListResponse(list.data(), 19, &confirmed));
}

TEST(SocialRoster, FeedMatchesTheSymbolNamesTheGameLogsWithoutTheSnsPrefix) {
  SocialRoster::Roster roster;
  const auto list = ListResponsePayload(0, 0, 1);
  const auto notify = StatusNotifyPayload(5, SocialRoster::kStatusOnline);
  EXPECT_TRUE(SocialRoster::Feed(roster, "FriendListResponse", list.data(), list.size()));
  EXPECT_TRUE(SocialRoster::Feed(roster, "FriendStatusNotify", notify.data(), notify.size()));
  EXPECT_EQ(roster.Count(), 1u);
  EXPECT_EQ(roster.Online(), 1u);
  EXPECT_FALSE(SocialRoster::Feed(roster, "SNSFriendStatusNotify", notify.data(), notify.size()));
  EXPECT_FALSE(SocialRoster::Feed(roster, "PartyJoinSuccess", notify.data(), notify.size()));
  EXPECT_FALSE(SocialRoster::Feed(roster, nullptr, notify.data(), notify.size()));
}

TEST(SocialRoster, ListFillsOnlineFirstAndLiveNotifiesUpdateIt) {
  SocialRoster::Roster roster;
  roster.BeginList(3);
  roster.Notify(30, SocialRoster::kStatusOffline);
  roster.Notify(20, SocialRoster::kStatusOnline);
  roster.Notify(10, SocialRoster::kStatusOffline);
  EXPECT_EQ(roster.Count(), 3u);
  EXPECT_EQ(roster.Online(), 1u);
  EXPECT_EQ(roster.Offline(), 2u);
  std::uint64_t id = 0;
  ASSERT_TRUE(roster.IdAt(0, &id));
  EXPECT_EQ(id, 20u);  // the online friend leads
  ASSERT_TRUE(roster.IdAt(1, &id));
  EXPECT_EQ(id, 10u);
  EXPECT_TRUE(roster.OnlineAt(0));
  EXPECT_FALSE(roster.OnlineAt(1));
  EXPECT_STREQ(roster.NameAt(0), "20");  // no name from the server yet: the id stands in

  roster.Notify(10, SocialRoster::kStatusOnline);  // a live change, outside a list
  EXPECT_EQ(roster.Count(), 3u);
  EXPECT_EQ(roster.Online(), 2u);
  roster.SetName(10, "ten");
  ASSERT_TRUE(roster.IdAt(0, &id));
  EXPECT_EQ(id, 10u);
  EXPECT_STREQ(roster.NameAt(0), "ten");
}

TEST(SocialRoster, ARefreshReplacesTheListAndAnEmptyOneClearsIt) {
  SocialRoster::Roster roster;
  roster.BeginList(2);
  roster.Notify(1, SocialRoster::kStatusOnline);
  roster.Notify(2, SocialRoster::kStatusOnline);
  ASSERT_EQ(roster.Count(), 2u);

  roster.BeginList(1);
  EXPECT_EQ(roster.Count(), 2u) << "the old list stays until the first new entry arrives";
  roster.Notify(2, SocialRoster::kStatusOffline);
  EXPECT_EQ(roster.Count(), 1u);
  EXPECT_EQ(roster.Online(), 0u);

  roster.BeginList(0);
  EXPECT_EQ(roster.Count(), 0u);
  EXPECT_STREQ(roster.NameAt(0), "");
  std::uint64_t id = 7;
  EXPECT_FALSE(roster.IdAt(0, &id));
}

TEST(SocialFacade, FriendSlotsAnswerFromTheRoster) {
  SocialRoster::Global().Clear();
  SocialRoster::Global().BeginList(2);
  SocialRoster::Global().Notify(99, SocialRoster::kStatusOffline);
  SocialRoster::Global().Notify(42, SocialRoster::kStatusOnline);

  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  using CountFn = std::uint32_t (*)(void*);
  using IdFn = std::uint64_t* (*)(void*, std::uint64_t*, std::uint32_t);
  using NameFn = const char* (*)(void*, std::uint32_t);
  using IndexFn = std::uint32_t (*)(void*, std::uint32_t);
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[46])(object), 2u);
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[47])(object), 1u);
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[48])(object), 1u);
  std::uint64_t id = 0;
  EXPECT_EQ(reinterpret_cast<IdFn>(vtable[49])(object, &id, 0), &id);
  EXPECT_EQ(id, 42u);
  EXPECT_STREQ(reinterpret_cast<NameFn>(vtable[50])(object, 0), "42");
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[51])(object, 0), 2u);  // online
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[51])(object, 1), 0u);  // offline
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[53])(object, 0), 0u) << "no party: nobody is invitable";
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[53])(object, 1), 0u);
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[54])(object, 0), 0u);  // nobody is joinable yet
  SocialRoster::Global().Clear();
}


std::string Hex(const std::uint8_t* data, std::size_t len) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 15]);
  }
  return out;
}

std::string U64s(std::initializer_list<std::uint64_t> values) {
  std::string out;
  for (const std::uint64_t v : values) SocialParty::AppendLe(out, v, 8);
  return out;
}

bool FeedParty(SocialParty::State& state, const char* name, const std::string& payload,
               std::vector<SocialParty::Message>* out = nullptr) {
  return state.Feed(SocialParty::ReplySymbol(name), reinterpret_cast<const std::uint8_t*>(payload.data()),
                    payload.size(), 1000, out);
}

std::uint64_t LastU64(const std::string& payload) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<std::uint8_t>(payload[payload.size() - 8 + i]);
  return v;
}

TEST(SocialParty, TheReplyTableNamesEachHashTheServerSends) {
  EXPECT_STREQ(SocialParty::ReplyName(0x0b7ac20124523993ULL), "PartyCreateSuccess");
  EXPECT_STREQ(SocialParty::ReplyName(0x218f721f09026dabULL), "PartyInviteNotify");
  EXPECT_EQ(SocialParty::ReplyName(0x0b7bd21332523994ULL), nullptr) << "that is our own create request";
  EXPECT_STREQ(SocialParty::RequestName(SocialParty::kCreateRequest), "PartyCreateRequest");
  EXPECT_EQ(SocialParty::ReplySymbol("PartyJoinSuccess"), 0xb57a32de4552e00bULL);
  EXPECT_EQ(SocialParty::ReplySymbol("Nope"), 0u);
  std::size_t count = 0;
  SocialParty::ReplyTable(&count);
  EXPECT_EQ(count, 28u);
}

// A real zstd frame (zstd CLI) of {"displayname":"Bob","x":1}, in a profile reply's shape.
std::string ProfileReply(std::uint64_t accountId, const std::string& frame) {
  std::string payload;
  SocialParty::AppendLe(payload, SocialNames::kPlatformOvrOrg, 8);
  SocialParty::AppendLe(payload, accountId, 8);
  SocialParty::AppendLe(payload, 27, 4);  // the length word the server writes before the stream
  return payload + frame;
}

const unsigned char kBobFrame[] = {0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x58, 0xd9, 0x00, 0x00, 0x7b, 0x22, 0x64, 0x69, 0x73,
                                   0x70, 0x6c, 0x61, 0x79, 0x6e, 0x61, 0x6d, 0x65, 0x22, 0x3a, 0x22, 0x42, 0x6f, 0x62,
                                   0x22, 0x2c, 0x22, 0x78, 0x22, 0x3a, 0x31, 0x7d, 0xb4, 0xfb, 0x07, 0x17};

TEST(SocialNames, ADisplayNameIsReadFromAProfileReply) {
  ASSERT_NE(SocialNames::DecoderSlot().load(), nullptr) << "social_names.cpp registers the decoder";
  const std::string reply =
      ProfileReply(695081603180789771ULL, std::string(reinterpret_cast<const char*>(kBobFrame), sizeof(kBobFrame)));
  std::uint64_t id = 0;
  std::string name;
  ASSERT_TRUE(SocialNames::DecodeProfile(reinterpret_cast<const std::uint8_t*>(reply.data()), reply.size(), &id, &name));
  EXPECT_EQ(id, 695081603180789771ULL);
  EXPECT_EQ(name, "Bob");

  // A truncated frame, a frame that is not zstd, and a reply too short to hold the header all fail.
  const std::string cut = ProfileReply(1, std::string(reinterpret_cast<const char*>(kBobFrame), 12));
  EXPECT_FALSE(SocialNames::DecodeProfile(reinterpret_cast<const std::uint8_t*>(cut.data()), cut.size(), &id, &name));
  const std::string junk = ProfileReply(1, "not a zstd frame at all");
  EXPECT_FALSE(SocialNames::DecodeProfile(reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size(), &id, &name));
  EXPECT_FALSE(SocialNames::DecodeProfile(reinterpret_cast<const std::uint8_t*>(reply.data()), 10, &id, &name));
}

TEST(SocialNames, TheRequestIsTheGamesOwnProfileRequestAndIsSentOncePerFriend) {
  const SocialParty::Message request = SocialNames::BuildProfileRequest(0x1122334455667788ULL);
  EXPECT_EQ(request.symbol, 0x1231172031050cb2ULL);
  ASSERT_EQ(request.payload.size(), 16u + 3u);
  EXPECT_EQ(static_cast<std::uint8_t>(request.payload[0]), 4) << "the platform the game logs in as";
  EXPECT_EQ(static_cast<std::uint8_t>(request.payload[8]), 0x88);
  EXPECT_EQ(request.payload.substr(16), std::string("{}\0", 3));

  SocialNames::Resolver resolver;
  EXPECT_EQ(resolver.Want(5).size(), 1u);
  EXPECT_TRUE(resolver.Want(5).empty()) << "already asked";
  EXPECT_TRUE(resolver.Want(0).empty());
  resolver.Reset();
  EXPECT_EQ(resolver.Want(5).size(), 1u);
}

TEST(SocialRoster, OverlappingRefreshesAndEarlyRepliesNeverLoseAName) {
  SocialRoster::Roster roster;
  roster.SetName(2, "Two");  // a profile reply that beats its friend into the roster
  roster.BeginList(3);
  roster.Notify(1, SocialRoster::kStatusOnline);
  roster.SetName(1, "One");
  roster.Notify(2, SocialRoster::kStatusOnline);
  roster.Notify(3, SocialRoster::kStatusOffline);
  roster.SetName(3, "Three");

  // One tab open starts several refreshes in a row; each begins with a partial roster.
  roster.BeginList(3);
  roster.Notify(3, SocialRoster::kStatusOffline);
  roster.BeginList(3);
  roster.Notify(1, SocialRoster::kStatusOnline);
  roster.Notify(2, SocialRoster::kStatusOnline);
  roster.Notify(3, SocialRoster::kStatusOffline);
  ASSERT_EQ(roster.Count(), 3u);
  EXPECT_STREQ(roster.NameAt(0), "One");
  EXPECT_STREQ(roster.NameAt(1), "Two");
  EXPECT_STREQ(roster.NameAt(2), "Three");
}

TEST(SocialRoster, ARefreshKeepsANameTheRosterAlreadyHas) {
  SocialRoster::Roster roster;
  roster.BeginList(1);
  roster.Notify(9, SocialRoster::kStatusOnline);
  roster.SetName(9, "Nine");
  roster.BeginList(1);
  roster.Notify(9, SocialRoster::kStatusOnline);
  EXPECT_STREQ(roster.NameAt(0), "Nine") << "a refresh must not turn a known name back into an id";
}

TEST(SocialParty, HashesMatchTheReferenceVectors) {
  const auto sha = SocialParty::Sha1("abc");
  EXPECT_EQ(Hex(sha.data(), sha.size()), "a9993e364706816aba3e25717850c26c9cd0d89d");
  const auto a = SocialParty::UuidV5Nil("OVR-ORG-695081603180789771");
  EXPECT_EQ(Hex(a.data(), a.size()), "819eb318e3865430b167151aae327cbb");
  const auto b = SocialParty::MemberUuid(1);
  EXPECT_EQ(Hex(b.data(), b.size()), "9b22f96a232a5571b27ff9e0f3824921");
}

TEST(SocialParty, RequestsHaveTheLengthsAndFieldsNakamaReads) {
  const SocialParty::Uuid self = SocialParty::MemberUuid(1);
  const auto invite = SocialParty::Standard(SocialParty::kInviteRequest, self, 0x1122334455667788ULL);
  EXPECT_EQ(invite.payload.size(), 40u);
  EXPECT_EQ(LastU64(invite.payload), 0x1122334455667788ULL);
  EXPECT_EQ(std::memcmp(invite.payload.data() + 8, self.data(), 16), 0);
  const auto respond = SocialParty::Targeted(SocialParty::kInviteResponse, self, SocialParty::MemberUuid(2), 1);
  EXPECT_EQ(respond.payload.size(), 48u);
  EXPECT_EQ(static_cast<std::uint8_t>(respond.payload[40]), 1);
  const std::string frame = SocialParty::Frame(invite);
  ASSERT_EQ(frame.size(), 24u + 40u);
  EXPECT_EQ(static_cast<std::uint8_t>(frame[0]), 0xf6);
  EXPECT_EQ(static_cast<std::uint8_t>(frame[16]), 40);
}

TEST(SocialParty, InvitingWithoutAPartyCreatesItFirstThenInvites) {
  SocialParty::State state;
  state.SetSelf(100);
  auto out = state.SendInvite(200);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kCreateRequest);
  EXPECT_EQ(state.SendInvite(300).size(), 0u) << "a second invite waits for the same create";
  EXPECT_TRUE(state.Snapshot().creating);

  std::vector<SocialParty::Message> outgoing;
  ASSERT_TRUE(FeedParty(state, "PartyCreateSuccess", U64s({7, 100}), &outgoing));
  ASSERT_EQ(outgoing.size(), 2u);
  EXPECT_EQ(outgoing[0].symbol, SocialParty::kInviteRequest);
  EXPECT_EQ(LastU64(outgoing[0].payload), 200u);
  EXPECT_EQ(LastU64(outgoing[1].payload), 300u);
  const auto view = state.Snapshot();
  EXPECT_EQ(view.partyId, 7u);
  EXPECT_FALSE(view.creating);
  ASSERT_EQ(view.members.size(), 1u);
  EXPECT_EQ(view.members[0].id, 100u);
  const auto events = state.DrainEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kCreated);

  out = state.SendInvite(400);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kInviteRequest) << "with a party the invite goes straight out";
}

TEST(SocialFacade, FriendIdAndNameFollowTheirIndexArgument) {
  SocialRoster::Global().Clear();
  SocialRoster::Global().BeginList(3);
  SocialRoster::Global().Notify(300, SocialRoster::kStatusOffline);
  SocialRoster::Global().Notify(100, SocialRoster::kStatusOnline);
  SocialRoster::Global().Notify(200, SocialRoster::kStatusOnline);
  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  using IdFn = std::uint64_t* (*)(void*, std::uint64_t*, std::uint32_t);
  using NameFn = const char* (*)(void*, std::uint32_t);
  const std::uint64_t expected[3] = {100, 200, 300};  // online first, then by id
  const char* names[3] = {"100", "200", "300"};
  for (std::uint32_t i = 0; i < 3; ++i) {
    std::uint64_t id = 0;
    reinterpret_cast<IdFn>(vtable[49])(object, &id, i);
    EXPECT_EQ(id, expected[i]) << "index " << i;
    EXPECT_STREQ(reinterpret_cast<NameFn>(vtable[50])(object, i), names[i]) << "index " << i;
  }
  std::uint64_t past = 7;
  reinterpret_cast<IdFn>(vtable[49])(object, &past, 3);
  EXPECT_EQ(past, 0u) << "an index past the list answers 0";
  SocialRoster::Global().Clear();
}

TEST(SocialParty, OpeningTheFriendsTabAsksTheServerForAFreshList) {
  SocialParty::State state;
  state.SetSelf(100);
  const auto out = state.RefreshFriends();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kFriendListRefreshRequest);
  EXPECT_EQ(out[0].payload.size(), 32u) << "the 0x20-byte shape Nakama reads";
  const auto self = SocialParty::MemberUuid(100);
  EXPECT_EQ(std::memcmp(out[0].payload.data() + 8, self.data(), 16), 0);
}

TEST(SocialFacade, TheLocalUserIsMemberZeroBeforeAnyPartyExists) {
  SocialParty::Global().SetSelf(77, "Me");
  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  std::uint8_t flags = 0;
  using UpdateFn = void (*)(void*, const void*);
  using CountFn = std::uint32_t (*)(void*);
  using IdFn = std::uint64_t* (*)(void*, std::uint64_t*, std::uint32_t);
  using NameFn = const char* (*)(void*, std::uint32_t);
  using HostFn = std::uint64_t* (*)(void*, std::uint64_t*);
  reinterpret_cast<UpdateFn>(vtable[13])(object, &flags);  // the per-frame Update publishes the view
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[26])(object), 1u) << "the UI indexes member 0";
  std::uint64_t id = 0;
  reinterpret_cast<IdFn>(vtable[27])(object, &id, 0);
  EXPECT_EQ(id, 77u);
  EXPECT_STREQ(reinterpret_cast<NameFn>(vtable[28])(object, 0), "Me");
  std::uint64_t host = 0;
  reinterpret_cast<HostFn>(vtable[23])(object, &host);
  EXPECT_EQ(host, 77u);
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[20])(object), 0u) << "there is still no party";
}

TEST(SocialParty, JoinByIdSendsTheJoinRequestAndDropsInvitesToThatParty) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({5, 201})));
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({6, 202})));
  const auto out = state.Join(5);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kJoinRequest);
  EXPECT_EQ(LastU64(out[0].payload), 5u);
  EXPECT_TRUE(state.Snapshot().joining);
  ASSERT_EQ(state.Snapshot().invites.size(), 1u);
  EXPECT_EQ(state.Snapshot().invites[0].partyId, 6u) << "the invite to the joined party is gone";
  EXPECT_TRUE(state.Join(5).empty()) << "a join is already in flight";
  ASSERT_TRUE(FeedParty(state, "PartyJoinSuccess", U64s({5, 201})));
  EXPECT_TRUE(state.Join(5).empty()) << "already in that party";
  EXPECT_TRUE(state.Join(0).empty());
}

TEST(SocialParty, ResetLeavesTheServerPartyWithoutTellingTheGame) {
  SocialParty::State state;
  state.SetSelf(100);
  EXPECT_TRUE(state.ResetParty().empty()) << "no party, nothing to leave";
  FeedParty(state, "PartyJoinSuccess", U64s({9, 201}));
  state.DrainEvents();
  const auto out = state.ResetParty();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kLeaveRequest);
  EXPECT_EQ(state.Snapshot().partyId, 0u);
  const auto events = state.DrainEvents();
  for (const auto& event : events) EXPECT_NE(event.kind, SocialParty::EventKind::kLeft) << "Reset fires no Left callback";
}

TEST(SocialFacade, AFriendRowIsInvitableOnlyWhileThePartyIsJoinableAndTheFriendIsNotInIt) {
  using CountFn = std::uint32_t (*)(void*);
  using IndexFn = std::uint32_t (*)(void*, std::uint32_t);
  using UpdateFn = void (*)(void*, const void*);
  SocialRoster::Global().Clear();
  SocialRoster::Global().BeginList(2);
  SocialRoster::Global().Notify(300, SocialRoster::kStatusOnline);
  SocialRoster::Global().Notify(400, SocialRoster::kStatusOffline);
  SocialParty::Global().SetSelf(77, "Me");
  SocialParty::Global().ResetParty();
  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  std::uint8_t flags = 0;
  const auto publish = [&] { reinterpret_cast<UpdateFn>(vtable[13])(object, &flags); };

  publish();
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[53])(object, 0), 0u) << "no party: no row shows the plus";

  FeedParty(SocialParty::Global(), "PartyCreateSuccess", U64s({7, 77}));
  publish();
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[53])(object, 0), 1u) << "an online friend, joinable party";
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[53])(object, 1), 0u) << "an offline friend";
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[53])(object, 9), 0u) << "past the list";

  FeedParty(SocialParty::Global(), "PartyJoinNotify", U64s({7, 300}));
  publish();
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[53])(object, 0), 0u) << "already in the party";
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[26])(object), 2u);

  SocialParty::Global().ResetParty();
  SocialRoster::Global().Clear();
  publish();
}

TEST(SocialParty, TheGamesCreateRequestMakesOnePartyAndNoMore) {
  SocialParty::State state;
  state.SetSelf(100);
  const auto first = state.CreateParty();
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].symbol, SocialParty::kCreateRequest);
  EXPECT_TRUE(state.CreateParty().empty()) << "a create is already in flight";
  ASSERT_TRUE(FeedParty(state, "PartyCreateSuccess", U64s({7, 100})));
  EXPECT_TRUE(state.CreateParty().empty()) << "the party exists";
  EXPECT_EQ(state.SendInvite(200).size(), 1u) << "with the party made, an invite goes straight out";
}

TEST(SocialParty, AcceptingTheNewestInviteJoinsThatParty) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({5, 201})));
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({6, 202})));
  EXPECT_EQ(state.Snapshot().invites.size(), 2u);
  EXPECT_EQ(state.DrainEvents().size(), 2u);

  const auto out = state.Accept(0);  // the game lists newest first
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kInviteResponse);
  const auto inviter = SocialParty::MemberUuid(202);
  EXPECT_EQ(std::memcmp(out[0].payload.data() + 16, inviter.data(), 16), 0);
  EXPECT_EQ(static_cast<std::uint8_t>(out[0].payload[40]), 1);
  EXPECT_TRUE(state.Snapshot().joining);
  EXPECT_EQ(state.Snapshot().invites.size(), 1u);

  ASSERT_TRUE(FeedParty(state, "PartyJoinSuccess", U64s({6, 202})));
  const auto view = state.Snapshot();
  EXPECT_EQ(view.partyId, 6u);
  EXPECT_EQ(view.ownerId, 202u);
  ASSERT_EQ(view.members.size(), 2u);
  EXPECT_EQ(view.members[0].id, 100u);
  EXPECT_EQ(view.members[1].id, 202u);
  const auto events = state.DrainEvents();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kJoined);
  EXPECT_EQ(events[1].kind, SocialParty::EventKind::kMemberJoined);
  EXPECT_EQ(events[1].index, 1u);
}

TEST(SocialParty, DismissingAnInviteRejectsItWithoutJoining) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({5, 201})));
  const auto out = state.Dismiss(0);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(static_cast<std::uint8_t>(out[0].payload[40]), 0);
  EXPECT_FALSE(state.Snapshot().joining);
  EXPECT_TRUE(state.Snapshot().invites.empty());
  EXPECT_TRUE(state.Accept(0).empty()) << "an index past the list does nothing";
}

TEST(SocialParty, MembersComeAndGoAndTheHostFollowsTheLeader) {
  SocialParty::State state;
  state.SetSelf(100);
  FeedParty(state, "PartyJoinSuccess", U64s({9, 201}));
  state.DrainEvents();
  ASSERT_TRUE(FeedParty(state, "PartyJoinNotify", U64s({9, 202})));
  ASSERT_TRUE(FeedParty(state, "PartyJoinNotify", U64s({9, 202}))) << "a repeat adds nothing";
  ASSERT_TRUE(FeedParty(state, "PartyJoinNotify", U64s({99, 203}))) << "another party's notify is ignored";
  EXPECT_EQ(state.Snapshot().members.size(), 3u);
  state.DrainEvents();

  ASSERT_TRUE(FeedParty(state, "PartyLeaveNotify", U64s({9, 201})));  // the leader leaves
  const auto view = state.Snapshot();
  ASSERT_EQ(view.members.size(), 2u);
  EXPECT_EQ(view.ownerId, 202u) << "the oldest remaining member leads";
  const auto events = state.DrainEvents();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kMemberLeft);
  EXPECT_EQ(events[0].id, 201u);
  EXPECT_EQ(events[1].kind, SocialParty::EventKind::kHostChanged);
}

TEST(SocialParty, OnlyTheLeaderCanKickOrPassAndAKickOfTheLocalUserEndsTheParty) {
  SocialParty::State state;
  state.SetSelf(100);
  FeedParty(state, "PartyJoinSuccess", U64s({9, 201}));
  FeedParty(state, "PartyJoinNotify", U64s({9, 202}));
  EXPECT_TRUE(state.Kick(1).empty()) << "not the leader";
  EXPECT_TRUE(state.Pass(2).empty());

  ASSERT_TRUE(FeedParty(state, "PartyPassNotify", U64s({9, 100})));
  EXPECT_EQ(state.Snapshot().ownerId, 100u);
  state.DrainEvents();
  const auto kick = state.Kick(2);
  ASSERT_EQ(kick.size(), 1u);
  EXPECT_EQ(kick[0].symbol, SocialParty::kKickRequest);
  const auto target = SocialParty::MemberUuid(202);
  EXPECT_EQ(std::memcmp(kick[0].payload.data() + 16, target.data(), 16), 0);
  EXPECT_EQ(state.Snapshot().members.size(), 2u);
  EXPECT_TRUE(state.Kick(0).empty()) << "the leader cannot kick themselves";

  const auto pass = state.Pass(1);
  ASSERT_EQ(pass.size(), 1u);
  EXPECT_EQ(pass[0].symbol, SocialParty::kPassRequest);
  EXPECT_EQ(state.Snapshot().ownerId, 201u);

  ASSERT_TRUE(FeedParty(state, "PartyKickNotify", U64s({9, 100})));
  EXPECT_EQ(state.Snapshot().partyId, 0u);
  const auto events = state.DrainEvents();
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().kind, SocialParty::EventKind::kKicked);
}

TEST(SocialParty, LeavingTellsTheServerFiresMemberLeftThenLeftAndALonePartyIsLeftAlone) {
  SocialParty::State state;
  state.SetSelf(100);
  EXPECT_TRUE(state.Leave().empty()) << "nothing to leave";
  FeedParty(state, "PartyCreateSuccess", U64s({3, 100}));
  state.DrainEvents();
  EXPECT_TRUE(state.Leave().empty()) << "pnsovr's Leave does nothing for a party of one";
  EXPECT_EQ(state.Snapshot().partyId, 3u);

  FeedParty(state, "PartyJoinNotify", U64s({3, 201}));
  FeedParty(state, "PartyJoinNotify", U64s({3, 202}));
  state.DrainEvents();
  const auto out = state.Leave();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kLeaveRequest);
  EXPECT_EQ(state.Snapshot().partyId, 0u);
  EXPECT_TRUE(state.Snapshot().members.empty());
  const auto events = state.DrainEvents();
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kMemberLeft);
  EXPECT_EQ(events[0].id, 202u) << "highest index first";
  EXPECT_EQ(events[1].kind, SocialParty::EventKind::kMemberLeft);
  EXPECT_EQ(events[1].id, 201u);
  EXPECT_EQ(events[2].kind, SocialParty::EventKind::kLeft);
}

TEST(SocialParty, BeingKickedFiresMemberLeftForEveryoneThenKickedOnlyIfThereWereOthers) {
  SocialParty::State state;
  state.SetSelf(100);
  FeedParty(state, "PartyJoinSuccess", U64s({9, 201}));
  FeedParty(state, "PartyJoinNotify", U64s({9, 202}));
  state.DrainEvents();
  ASSERT_TRUE(FeedParty(state, "PartyKickNotify", U64s({9, 100})));
  const auto events = state.DrainEvents();
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kMemberLeft);
  EXPECT_EQ(events[0].id, 202u);
  EXPECT_EQ(events[1].id, 201u);
  EXPECT_EQ(events[2].kind, SocialParty::EventKind::kKicked);

  SocialParty::State alone;
  alone.SetSelf(100);
  FeedParty(alone, "PartyCreateSuccess", U64s({4, 100}));
  alone.DrainEvents();
  ASSERT_TRUE(FeedParty(alone, "PartyKickNotify", U64s({4, 100})));
  EXPECT_TRUE(alone.DrainEvents().empty()) << "no remote members: pnsovr fires nothing";
}

TEST(SocialParty, JoinFailureCodesAreTheGamesAndAnInviteFromTheSameSenderReplacesTheOlder) {
  SocialParty::State state;
  state.SetSelf(100);
  std::string refused = U64s({5});
  refused.push_back(2);
  ASSERT_TRUE(FeedParty(state, "PartyJoinFailure", refused));
  auto events = state.DrainEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kJoinFailed);
  EXPECT_EQ(events[0].code, 4u) << "refused maps to the game's 'not joinable'";

  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({5, 201})));
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({8, 201})));
  const auto view = state.Snapshot();
  ASSERT_EQ(view.invites.size(), 1u) << "one invite per sender";
  EXPECT_EQ(view.invites[0].partyId, 8u) << "the newer one wins";
}

TEST(SocialParty, OnlyPartyMessagesAreHandled) {
  SocialParty::State state;
  EXPECT_FALSE(FeedParty(state, "FriendStatusNotify", U64s({1, 2})));
  EXPECT_FALSE(state.Feed(0, nullptr, 0, 0, nullptr));
  EXPECT_FALSE(FeedParty(state, "NotARealMessage", U64s({1, 2}))) << "an unknown symbol is not a party message";
  EXPECT_TRUE(FeedParty(state, "PartyLeaveSuccess", std::string(1, '\0'))) << "a reply with nothing to show";
  EXPECT_TRUE(state.DrainEvents().empty());
}

}  // namespace

TEST(PartyInviteGate, OnlyTheFirstMatchFlagIsForcedTrue) {
  EXPECT_EQ(PartyInviteGate::BooleanResult("npe|firstmatch|completed", 0), 1u);
  EXPECT_EQ(PartyInviteGate::BooleanResult("npe|firstmatch|completed", 1), 1u);
  EXPECT_EQ(PartyInviteGate::BooleanResult("npe|firstmatch|other", 0), 0u);
  EXPECT_EQ(PartyInviteGate::BooleanResult("npe|firstmatch|completed|x", 0), 0u);
  EXPECT_EQ(PartyInviteGate::BooleanResult("other", 1), 1u);
  EXPECT_EQ(PartyInviteGate::BooleanResult(nullptr, 0), 0u);
}
