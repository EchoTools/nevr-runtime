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
#include "runtime/patch/provider_identity.h"
#include "runtime/patch/social_facade.h"
#include "runtime/scenario/scenario_protocol.h"
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

// Issue #57: the tab-open refresh (slot 45) is answered with a fresh FriendListResponse plus one
// FriendStatusNotify per friend, and that answer replaces the roster. The refresh here lists a
// different set than the login did (friend 22 gone, friend 77 new), so upserting notifies alone
// would give {11, 22, 77}; only a roster rebuilt from the refresh list gives {77, 11}. The bytes
// go through SocialRoster::Feed, the call the ws bridge makes for every server->game message, and
// are read back through the facade slots the tab reads.
TEST(SocialFacade, TheRefreshAnswerReplacesTheRosterWithTheServersCurrentFriends) {
  SocialRoster::Global().Clear();
  const auto feed = [](const char* name, const std::uint8_t* data, std::size_t len) {
    ASSERT_TRUE(SocialRoster::Feed(SocialRoster::Global(), name, data, len)) << name;
  };
  // Login: friends 11 and 22, both offline.
  const auto loginList = ListResponsePayload(2, 0, 0);
  feed("FriendListResponse", loginList.data(), loginList.size());
  for (const std::uint64_t id : {11ULL, 22ULL}) {
    const auto notify = StatusNotifyPayload(id, SocialRoster::kStatusOffline);
    feed("FriendStatusNotify", notify.data(), notify.size());
  }

  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  using CountFn = std::uint32_t (*)(void*);
  using IdFn = std::uint64_t* (*)(void*, std::uint64_t*, std::uint32_t);
  const auto idAt = [&](std::uint32_t index) {
    std::uint64_t id = 0;
    reinterpret_cast<IdFn>(vtable[49])(object, &id, index);
    return id;
  };
  ASSERT_EQ(reinterpret_cast<CountFn>(vtable[46])(object), 2u);
  ASSERT_EQ(idAt(0), 11u);
  ASSERT_EQ(idAt(1), 22u);

  // The refresh's list announces two friends: 11 offline and 77 online. Until the first entry
  // arrives the previous roster stays visible, unchanged.
  const auto refreshList = ListResponsePayload(1, 0, 1);
  feed("FriendListResponse", refreshList.data(), refreshList.size());
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[46])(object), 2u) << "the old roster stays until the refresh's entries arrive";
  EXPECT_EQ(idAt(0), 11u);
  EXPECT_EQ(idAt(1), 22u);

  const auto added = StatusNotifyPayload(77, SocialRoster::kStatusOnline);
  feed("FriendStatusNotify", added.data(), added.size());
  const auto kept = StatusNotifyPayload(11, SocialRoster::kStatusOffline);
  feed("FriendStatusNotify", kept.data(), kept.size());

  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[46])(object), 2u) << "friend 22 is not in the refresh, so it is gone";
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[47])(object), 1u);
  EXPECT_EQ(idAt(0), 77u) << "online friends lead the list";
  EXPECT_EQ(idAt(1), 11u);
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

TEST(SocialParty, TheFriendsTabRefreshIsRateLimited) {
  SocialParty::State state;
  state.SetSelf(100);
  const std::uint64_t window = SocialParty::State::kFriendRefreshMinSeconds;
  const std::uint64_t t0 = 1000;
  ASSERT_EQ(state.RefreshFriendsOnTabOpen(t0).size(), 1u) << "the first open asks";
  EXPECT_TRUE(state.RefreshFriendsOnTabOpen(t0).empty()) << "the same second asks nothing";
  EXPECT_TRUE(state.RefreshFriendsOnTabOpen(t0 + window - 1).empty()) << "inside the window asks nothing";
  const auto again = state.RefreshFriendsOnTabOpen(t0 + window);
  ASSERT_EQ(again.size(), 1u) << "at the window's end it asks again";
  EXPECT_EQ(again[0].symbol, SocialParty::kFriendListRefreshRequest);
  EXPECT_TRUE(state.RefreshFriendsOnTabOpen(t0 + window + 1).empty()) << "the window restarts at each request";
  EXPECT_EQ(state.RefreshFriends().size(), 1u) << "the server-driven re-request is not limited";
}

TEST(SocialParty, AFriendsTabHeldOpenIsRefreshedEveryPollInterval) {
  SocialParty::State state;
  state.SetSelf(100);
  const std::uint64_t t0 = 5000;
  const std::uint64_t poll = SocialParty::State::kFriendPollSeconds;
  ASSERT_EQ(state.RefreshFriendsOnTabOpen(t0).size(), 1u);
  // The tab reads the list every second for three poll intervals; Update polls every second.
  std::size_t sent = 0;
  for (std::uint64_t t = t0 + 1; t <= t0 + 3 * poll; ++t) {
    state.NoteFriendsViewed(t);
    const auto out = state.PollFriendsWhileOpen(t);
    sent += out.size();
    for (const auto& m : out) EXPECT_EQ(m.symbol, SocialParty::kFriendListRefreshRequest);
  }
  EXPECT_EQ(sent, 3u) << "one refresh per interval while the tab is held open";
}

TEST(SocialParty, AClosedFriendsTabIsNotPolled) {
  SocialParty::State state;
  state.SetSelf(100);
  const std::uint64_t t0 = 5000;
  EXPECT_TRUE(state.PollFriendsWhileOpen(t0).empty()) << "never opened";
  ASSERT_EQ(state.RefreshFriendsOnTabOpen(t0).size(), 1u);
  // The tab is read for 4 seconds and then closed: no read for the idle window.
  for (std::uint64_t t = t0 + 1; t <= t0 + 4; ++t) state.NoteFriendsViewed(t);
  std::size_t sent = 0;
  for (std::uint64_t t = t0 + 5; t <= t0 + 6 * SocialParty::State::kFriendPollSeconds; ++t) {
    sent += state.PollFriendsWhileOpen(t).size();
  }
  EXPECT_EQ(sent, 0u) << "a closed tab sends nothing, however long it stays closed";
}

TEST(SocialParty, ThePollNeverBeatsTheRefreshFloor) {
  SocialParty::State state;
  state.SetSelf(100);
  const std::uint64_t t0 = 5000;
  ASSERT_EQ(state.RefreshFriendsOnTabOpen(t0).size(), 1u);
  const std::uint64_t t1 = t0 + SocialParty::State::kFriendPollSeconds;
  state.NoteFriendsViewed(t1);
  ASSERT_EQ(state.PollFriendsWhileOpen(t1).size(), 1u);
  // A tab reopened a moment after the poll is inside the floor: nothing goes out.
  EXPECT_TRUE(state.RefreshFriendsOnTabOpen(t1 + SocialParty::State::kFriendRefreshMinSeconds - 1).empty());
  EXPECT_TRUE(state.PollFriendsWhileOpen(t1 + 1).empty());
  static_assert(SocialParty::State::kFriendPollSeconds >= SocialParty::State::kFriendRefreshMinSeconds,
                "the poll interval is never below the floor");
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

// Server-controlled: every PartyJoinNotify appends a member, but the game indexes a 10-entry member
// JSON array by the reported count (CR15NetGame::PartyMemberData), so the count is capped at the array.
TEST(SocialFacade, MemberCountNeverExceedsTheMemberJsonArray) {
  using CountFn = std::uint32_t (*)(void*);
  using IdFn = std::uint64_t* (*)(void*, std::uint64_t*, std::uint32_t);
  using UpdateFn = void (*)(void*, const void*);
  SocialRoster::Global().Clear();
  SocialParty::Global().SetSelf(77, "Me");
  SocialParty::Global().ResetParty();
  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  std::uint8_t flags = 0;
  const auto publish = [&] { reinterpret_cast<UpdateFn>(vtable[13])(object, &flags); };
  const std::uint32_t clampedBefore = SocialFacade::TestMembersClamped();

  FeedParty(SocialParty::Global(), "PartyCreateSuccess", U64s({7, 77}));
  for (std::uint64_t id = 301; id <= 311; ++id) FeedParty(SocialParty::Global(), "PartyJoinNotify", U64s({7, id}));
  ASSERT_EQ(SocialParty::Global().Snapshot().members.size(), 12U) << "the party model itself holds all twelve";
  publish();
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[26])(object), 10U);
  EXPECT_EQ(SocialFacade::TestMembersClamped(), clampedBefore + 1);
  std::uint64_t id = 0;
  reinterpret_cast<IdFn>(vtable[27])(object, &id, 9);
  EXPECT_EQ(id, 309U) << "index 9 is the last reported member";
  reinterpret_cast<IdFn>(vtable[27])(object, &id, 10);
  EXPECT_EQ(id, 0U) << "index 10 names nobody";

  publish();
  EXPECT_EQ(SocialFacade::TestMembersClamped(), clampedBefore + 1) << "an unchanged clamp is counted once";

  // The 10/11 boundary: exactly ten members is not clamped.
  SocialParty::Global().ResetParty();
  FeedParty(SocialParty::Global(), "PartyCreateSuccess", U64s({8, 77}));
  for (std::uint64_t member = 301; member <= 309; ++member) FeedParty(SocialParty::Global(), "PartyJoinNotify", U64s({8, member}));
  publish();
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[26])(object), 10U);

  SocialParty::Global().ResetParty();
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

  ASSERT_EQ(state.InvitePartyAt(0), 6u) << "the game lists newest first";
  ASSERT_TRUE(state.BeginJoin(6));
  const auto out = state.Join(6);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kInviteResponse);
  const auto inviter = SocialParty::MemberUuid(202);
  EXPECT_EQ(std::memcmp(out[0].payload.data() + 16, inviter.data(), 16), 0);
  EXPECT_EQ(static_cast<std::uint8_t>(out[0].payload[40]), 1);
  EXPECT_TRUE(state.Snapshot().joining);
  EXPECT_EQ(state.Snapshot().joiningPartyId, 6u);
  EXPECT_EQ(state.Snapshot().invites.size(), 1u);

  ASSERT_TRUE(FeedParty(state, "PartyJoinSuccess", U64s({6, 202})));
  EXPECT_EQ(state.Snapshot().joiningPartyId, 0u);
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
  EXPECT_EQ(state.InvitePartyAt(0), 0u) << "an index past the list names no party";
}

TEST(SocialParty, AJoinWithoutAnInviteIsAJoinRequestAndOneFromAnInviteIsItsAccept) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(state.BeginJoin(8));
  const auto plain = state.Join(8);
  ASSERT_EQ(plain.size(), 1u);
  EXPECT_EQ(plain[0].symbol, SocialParty::kJoinRequest);
  ASSERT_TRUE(FeedParty(state, "PartyJoinFailure", U64s({8, 1})));

  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({9, 203})));
  ASSERT_TRUE(state.BeginJoin(9)) << "the join button on a party there is an invite to";
  EXPECT_TRUE(state.Snapshot().invites.empty());
  const auto accept = state.Join(9);
  ASSERT_EQ(accept.size(), 1u);
  EXPECT_EQ(accept[0].symbol, SocialParty::kInviteResponse);
  const auto inviter = SocialParty::MemberUuid(203);
  EXPECT_EQ(std::memcmp(accept[0].payload.data() + 16, inviter.data(), 16), 0);
}

TEST(SocialParty, AJoinWhileAnotherIsInFlightIsDeferredAndRetried) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({5, 201})));
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({6, 202})));
  ASSERT_TRUE(state.BeginJoin(5));
  ASSERT_EQ(state.Join(5).size(), 1u);
  EXPECT_FALSE(state.BeginJoin(6)) << "a join is in flight";
  EXPECT_EQ(state.DeferredJoin(), 6u);
  EXPECT_TRUE(state.Snapshot().invites.empty()) << "the invite is dropped even when the join waits";

  ASSERT_TRUE(FeedParty(state, "PartyJoinFailure", U64s({5, 1})));
  ASSERT_TRUE(state.BeginJoin(state.DeferredJoin())) << "Update retries it once nothing is in flight";
  const auto out = state.Join(6);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, SocialParty::kInviteResponse) << "still the accept of the dropped invite";
  EXPECT_EQ(state.DeferredJoin(), 0u);
}

TEST(SocialParty, ARefusedJoinIsForgottenAndJoiningTheCurrentPartyDoesNothing) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({5, 201})));
  ASSERT_TRUE(state.BeginJoin(5));
  state.AbandonJoin(5);
  EXPECT_TRUE(state.Snapshot().invites.empty()) << "pnsovr drops the invite before the gate";
  EXPECT_EQ(state.DeferredJoin(), 0u);

  ASSERT_TRUE(FeedParty(state, "PartyJoinSuccess", U64s({7, 201})));
  state.DrainEvents();
  ASSERT_TRUE(state.BeginJoin(7));
  EXPECT_TRUE(state.Join(7).empty()) << "already in that party";
  EXPECT_TRUE(state.DrainEvents().empty());
}

TEST(SocialParty, AnAbandonedJoinGivesTheInviteBackAndFailsToTheGame) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({5, 201})));
  state.DrainEvents();
  ASSERT_TRUE(state.BeginJoin(5));
  const auto accept = state.Join(5);
  ASSERT_EQ(accept.size(), 1u);
  EXPECT_EQ(accept[0].symbol, SocialParty::kInviteResponse);
  EXPECT_EQ(accept[0].target, 201u) << "the request records the account it is aimed at, for logs";
  EXPECT_TRUE(state.Snapshot().invites.empty());

  EXPECT_TRUE(state.AbandonJoining()) << "the join was in flight";
  EXPECT_FALSE(state.Snapshot().joining);
  ASSERT_EQ(state.Snapshot().invites.size(), 1u) << "the invite the join consumed comes back";
  EXPECT_EQ(state.Snapshot().invites[0].senderId, 201u);
  const auto events = state.DrainEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kJoinFailed);
  EXPECT_EQ(events[0].code, 0u);
  EXPECT_FALSE(state.AbandonJoining()) << "a second call changes nothing";
  EXPECT_TRUE(state.DrainEvents().empty());

  // A retry by party id is the invite's accept again, not a plain join.
  ASSERT_TRUE(state.BeginJoin(5));
  const auto retry = state.Join(5);
  ASSERT_EQ(retry.size(), 1u);
  EXPECT_EQ(retry[0].symbol, SocialParty::kInviteResponse);

  // Once the server answers, nothing is left to restore.
  ASSERT_TRUE(FeedParty(state, "PartyJoinSuccess", U64s({5, 201})));
  EXPECT_FALSE(state.AbandonJoining());
  EXPECT_TRUE(state.Snapshot().invites.empty());
}

TEST(SocialParty, TargetedRequestsRecordTheAccountTheyAreAimedAt) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyCreateSuccess", U64s({7, 100})));
  ASSERT_TRUE(FeedParty(state, "PartyJoinNotify", U64s({7, 301})));
  ASSERT_TRUE(FeedParty(state, "PartyJoinNotify", U64s({7, 302})));
  const auto pass = state.Pass(2);
  ASSERT_EQ(pass.size(), 1u);
  EXPECT_EQ(pass[0].target, 302u);
  ASSERT_TRUE(FeedParty(state, "PartyPassNotify", U64s({7, 100})));
  const auto kick = state.Kick(1);
  ASSERT_EQ(kick.size(), 1u);
  EXPECT_EQ(kick[0].target, 301u);
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({9, 303})));
  const auto dismiss = state.Dismiss(0);
  ASSERT_EQ(dismiss.size(), 1u);
  EXPECT_EQ(dismiss[0].target, 303u);
}

TEST(SocialParty, AnInviteIsQueuedOncePerTargetBehindTheCreate) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_EQ(state.SendInvite(300).size(), 1u) << "the create";
  EXPECT_TRUE(state.AbandonCreate());
  EXPECT_FALSE(state.AbandonCreate());
  ASSERT_EQ(state.SendInvite(300).size(), 1u) << "the create again";
  std::vector<SocialParty::Message> outgoing;
  ASSERT_TRUE(FeedParty(state, "PartyCreateSuccess", U64s({7, 100}), &outgoing));
  std::size_t invites = 0;
  for (const auto& m : outgoing) invites += m.symbol == SocialParty::kInviteRequest ? 1 : 0;
  EXPECT_EQ(invites, 1u);
}

TEST(SocialParty, AnUnansweredLockIsForgottenOnlyWhileItIsUnanswered) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyCreateSuccess", U64s({7, 100})));
  ASSERT_EQ(state.SetLocked(true).size(), 1u);
  EXPECT_TRUE(state.SetLocked(true).empty()) << "asked once";
  EXPECT_FALSE(state.ExpireLockRequest(false)) << "a different request is not the one in flight";
  EXPECT_TRUE(state.ExpireLockRequest(true));
  EXPECT_EQ(state.SetLocked(true).size(), 1u) << "forgotten, so it may be asked again";
  ASSERT_TRUE(FeedParty(state, "PartyLockSuccess", U64s({7})));
  EXPECT_FALSE(state.ExpireLockRequest(true)) << "answered";
}

TEST(SocialParty, AcceptingAnInviteKeepsTheCurrentPartyUntilTheNewOneAdmits) {
  SocialParty::State state;
  state.SetSelf(100);
  ASSERT_TRUE(FeedParty(state, "PartyJoinSuccess", U64s({7, 201})));
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({9, 203})));
  state.DrainEvents();
  ASSERT_TRUE(state.BeginJoin(9));
  ASSERT_EQ(state.Join(9).size(), 1u);
  EXPECT_TRUE(state.DrainEvents().empty()) << "nothing leaves before the server answers";
  EXPECT_EQ(state.Snapshot().partyId, 7u) << "still in the party we were in";
  EXPECT_TRUE(state.Snapshot().joining);

  ASSERT_TRUE(FeedParty(state, "PartyJoinFailure", U64s({9, 5})));
  EXPECT_EQ(state.Snapshot().partyId, 7u) << "a failed join stays where it was";
  ASSERT_EQ(state.Snapshot().members.size(), 2u);
  auto events = state.DrainEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kJoinFailed);

  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({9, 203})));
  state.DrainEvents();
  ASSERT_TRUE(state.BeginJoin(9));
  ASSERT_EQ(state.Join(9).size(), 1u);
  ASSERT_TRUE(FeedParty(state, "PartyJoinSuccess", U64s({9, 203})));
  events = state.DrainEvents();
  ASSERT_EQ(events.size(), 4u) << "the old party is left only now, then the new one joined";
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kMemberLeft);
  EXPECT_EQ(events[0].id, 201u);
  EXPECT_EQ(events[1].kind, SocialParty::EventKind::kLeft);
  EXPECT_EQ(events[2].kind, SocialParty::EventKind::kJoined);
  EXPECT_EQ(events[3].kind, SocialParty::EventKind::kMemberJoined);
  EXPECT_EQ(state.Snapshot().partyId, 9u);
}

TEST(SocialParty, TheLeadersJoinPolicyGoesToTheServerAndFollowsANewParty) {
  SocialParty::State state;
  state.SetSelf(100);
  EXPECT_TRUE(state.SetJoinPolicy(0).empty()) << "no party: remembered, nothing sent";
  std::vector<SocialParty::Message> outgoing;
  ASSERT_TRUE(FeedParty(state, "PartyCreateSuccess", U64s({7, 100}), &outgoing));
  ASSERT_EQ(outgoing.size(), 1u) << "a new party gets the remembered policy";
  EXPECT_EQ(outgoing[0].symbol, SocialParty::kSetJoinPolicyRequest);
  EXPECT_EQ(LastU64(outgoing[0].payload), 0u);
  EXPECT_TRUE(state.SetJoinPolicy(0).empty()) << "unchanged";
  const auto out = state.SetJoinPolicy(1);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(LastU64(out[0].payload), 1u);
  EXPECT_STREQ(SocialParty::RequestName(SocialParty::kSetJoinPolicyRequest), "PartySetJoinPolicyRequest");

  SocialParty::State member;
  member.SetSelf(100);
  ASSERT_TRUE(FeedParty(member, "PartyJoinSuccess", U64s({8, 201})));
  EXPECT_TRUE(member.SetJoinPolicy(0).empty()) << "a member does not set the party's policy";
}

TEST(SocialFacade, AcceptInviteJoinsThatPartyAndIdShowsItWhileTheJoinIsInFlight) {
  using AcceptFn = void (*)(void*, std::int32_t);
  using CountFn = std::uint32_t (*)(void*);
  using IdFn = std::uint64_t (*)(void*);
  using UpdateFn = void (*)(void*, const void*);
  SocialParty::Global().SetSelf(77, "Me");
  SocialParty::Global().ResetParty();
  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  std::uint8_t flags = 0;
  ASSERT_TRUE(FeedParty(SocialParty::Global(), "PartyInviteNotify", U64s({51, 4242})));
  reinterpret_cast<UpdateFn>(vtable[13])(object, &flags);
  ASSERT_EQ(reinterpret_cast<CountFn>(vtable[70])(object), 1u);

  reinterpret_cast<AcceptFn>(vtable[73])(object, 0);  // nothing sends in tests; the state is what counts
  reinterpret_cast<UpdateFn>(vtable[13])(object, &flags);
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[70])(object), 0u);
  EXPECT_EQ(reinterpret_cast<IdFn>(vtable[25])(object), 51u) << "pnsovr's Id is the room being joined";
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[20])(object), 0u) << "not Ready while joining";

  ASSERT_TRUE(FeedParty(SocialParty::Global(), "PartyJoinFailure", U64s({51, 1})));
  reinterpret_cast<UpdateFn>(vtable[13])(object, &flags);
  EXPECT_EQ(reinterpret_cast<IdFn>(vtable[25])(object), 0u);
  SocialParty::Global().ResetParty();
  SocialParty::Global().DrainEvents();
}

TEST(SocialParty, MembersAndInviteSendersGetTheirDisplayNames) {
  SocialParty::State state;
  state.SetSelf(100, "Me");
  ASSERT_TRUE(FeedParty(state, "PartyCreateSuccess", U64s({7, 100})));
  EXPECT_EQ(state.Snapshot().members[0].name, "Me") << "the local user's own name, not its id";
  EXPECT_TRUE(state.TakeUnnamed().empty());

  ASSERT_TRUE(FeedParty(state, "PartyJoinNotify", U64s({7, 201})));
  ASSERT_TRUE(FeedParty(state, "PartyInviteNotify", U64s({9, 202})));
  EXPECT_EQ(state.Snapshot().members[1].name, "201") << "the id until a name arrives";
  EXPECT_EQ(state.TakeUnnamed(), (std::vector<std::uint64_t>{201, 202}));
  EXPECT_TRUE(state.TakeUnnamed().empty()) << "each id is handed out once";

  state.SetName(201, "Alice");
  state.SetName(202, "Bob");
  EXPECT_EQ(state.Snapshot().members[1].name, "Alice");
  EXPECT_EQ(state.Snapshot().invites[0].senderName, "Bob");

  ASSERT_TRUE(FeedParty(state, "PartyLeaveNotify", U64s({7, 201})));
  ASSERT_TRUE(FeedParty(state, "PartyJoinNotify", U64s({7, 201})));
  EXPECT_EQ(state.Snapshot().members[1].name, "Alice") << "a known name is used at once";
  EXPECT_TRUE(state.TakeUnnamed().empty());
}

TEST(SocialFacade, TheHostsLockBitLocksThePartyOnTheServerAndJoinableFollowsIt) {
  using CountFn = std::uint32_t (*)(void*);
  using UpdateFn = void (*)(void*, const void*);
  SocialParty::Global().SetSelf(77, "Me");
  SocialParty::Global().ResetParty();
  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  std::uint8_t flags = 0;
  ASSERT_TRUE(FeedParty(SocialParty::Global(), "PartyCreateSuccess", U64s({7, 77})));
  reinterpret_cast<UpdateFn>(vtable[13])(object, &flags);
  ASSERT_EQ(reinterpret_cast<CountFn>(vtable[22])(object), 1u) << "a fresh party is joinable";
  ASSERT_EQ(reinterpret_cast<CountFn>(vtable[4])(object), 1u) << "and not locked on the server";

  std::uint32_t word = 0;
  std::memcpy(&word, static_cast<std::uint8_t*>(object) + 0x27C, 4);
  word &= ~2u;  // what the game's PartyLock node does to the social object
  std::memcpy(static_cast<std::uint8_t*>(object) + 0x27C, &word, 4);
  reinterpret_cast<UpdateFn>(vtable[13])(object, &flags);
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[22])(object), 0u) << "the host's own bit decides at once";
  EXPECT_TRUE(SocialParty::Global().SetLocked(true).empty()) << "Update already asked the server to lock";

  ASSERT_TRUE(FeedParty(SocialParty::Global(), "PartyLockSuccess", U64s({7})));
  reinterpret_cast<UpdateFn>(vtable[13])(object, &flags);
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[4])(object), 0u) << "the server locked it";

  word |= 2u;
  std::memcpy(static_cast<std::uint8_t*>(object) + 0x27C, &word, 4);
  reinterpret_cast<UpdateFn>(vtable[13])(object, &flags);
  EXPECT_EQ(reinterpret_cast<CountFn>(vtable[22])(object), 1u);
  EXPECT_TRUE(SocialParty::Global().SetLocked(false).empty()) << "Update asked to unlock";
  SocialParty::Global().ResetParty();
  SocialParty::Global().DrainEvents();
}

TEST(SocialFacade, AFriendsPresenceFillsTheStatusTextAndJoinablePartySlots) {
  using TextFn = const char* (*)(void*, std::uint32_t);
  using JoinableFn = std::uint32_t (*)(void*, std::uint32_t);
  using PartyFn = std::uint64_t (*)(void*, std::uint32_t);
  const std::string frame = nevr_scenario_protocol::BuildFriendPresenceNotify(4242, 77, true, "Public Arena Match");
  std::uint64_t id = 0;
  SocialRoster::Presence presence;
  ASSERT_TRUE(SocialRoster::ParsePresenceNotify(reinterpret_cast<const std::uint8_t*>(frame.data()) + 24,
                                                frame.size() - 24, &id, &presence));
  EXPECT_EQ(id, 4242u);
  EXPECT_EQ(presence.partyId, 77u);
  EXPECT_TRUE(presence.joinable);
  EXPECT_EQ(presence.text, "Public Arena Match");
  EXPECT_FALSE(SocialRoster::ParsePresenceNotify(reinterpret_cast<const std::uint8_t*>(frame.data()) + 24, 40, &id,
                                                 &presence)) << "a text running past the payload is refused";

  SocialRoster::Global().Clear();
  SocialRoster::Global().SetPresence(4242, presence);  // before the friend is listed: remembered
  SocialRoster::Global().BeginList(1);
  SocialRoster::Global().Notify(4242, SocialRoster::kStatusOnline);
  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  EXPECT_STREQ(reinterpret_cast<TextFn>(vtable[52])(object, 0), "Public Arena Match");
  EXPECT_EQ(reinterpret_cast<JoinableFn>(vtable[54])(object, 0), 1u);
  EXPECT_EQ(reinterpret_cast<PartyFn>(vtable[55])(object, 0), 77u);
  EXPECT_STREQ(reinterpret_cast<TextFn>(vtable[52])(object, 5), "") << "past the list";

  SocialRoster::Presence notJoinable;
  notJoinable.text = "In Main Menu";
  SocialRoster::Global().SetPresence(4242, notJoinable);
  EXPECT_EQ(reinterpret_cast<PartyFn>(vtable[55])(object, 0), 0u);
  EXPECT_EQ(reinterpret_cast<JoinableFn>(vtable[54])(object, 0), 0u);
  SocialRoster::Global().Notify(4242, SocialRoster::kStatusOffline);
  SocialRoster::Global().SetPresence(4242, presence);
  EXPECT_EQ(reinterpret_cast<PartyFn>(vtable[55])(object, 0), 0u) << "an offline friend's party is not joinable";
  SocialRoster::Global().Clear();
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
  EXPECT_EQ(events[0].code, 4u) << "refused maps to the game's 'locked'";
  EXPECT_EQ(SocialParty::GameJoinFailureCode(1), 1u);
  EXPECT_EQ(SocialParty::GameJoinFailureCode(3), 3u);
  EXPECT_EQ(SocialParty::GameJoinFailureCode(5), 5u) << "full";
  EXPECT_EQ(SocialParty::GameJoinFailureCode(6), 6u);
  EXPECT_EQ(SocialParty::GameJoinFailureCode(9), 0u) << "unknown";

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

// Scenario control protocol (src/runtime/scenario/scenario_protocol.h). Pure, so it is covered in
// every build even though the endpoint itself only exists in the mingw-scenario preset.
TEST(ScenarioProtocol, ParsesTheThreeOps) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"state"})", &cmd, &error)) << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kState);

  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(
      R"({"op":"inject","msg":"FriendStatusNotify","id":4242,"status":0})", &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kInjectFriendStatus);
  EXPECT_EQ(cmd.friendId, 4242ULL);
  EXPECT_EQ(cmd.status, 0);

  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(
      R"({"op":"fire","action":"friend_invite","user":"OVR-ORG-4242"})", &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kFireFriendInvite);
  EXPECT_EQ(cmd.user, "OVR-ORG-4242");
}

TEST(ScenarioProtocol, RejectionsNameWhatWasWrong) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand("not json", &cmd, &error));
  EXPECT_NE(error.find("not a JSON object"), std::string::npos);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"kick"})", &cmd, &error));
  EXPECT_NE(error.find("unknown op \"kick\""), std::string::npos);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyKickRequest","id":1,"status":0})",
                                              &cmd, &error));
  EXPECT_NE(error.find("supports msg \"FriendStatusNotify\""), std::string::npos);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"FriendStatusNotify","id":0,"status":0})",
                                              &cmd, &error));
  EXPECT_NE(error.find("nonzero"), std::string::npos);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"FriendStatusNotify","id":5,"status":7})",
                                              &cmd, &error));
  EXPECT_NE(error.find("status"), std::string::npos);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"fire","action":"friend_invite"})", &cmd, &error));
  EXPECT_NE(error.find("\"user\""), std::string::npos);
}

// The injected frame must be one the bridge's own roster feed reads back, so an injected friend is
// the same thing as a friend the server announced.
TEST(ScenarioProtocol, FriendStatusNotifyFrameRoundTripsThroughTheRosterParser) {
  const std::string frame = nevr_scenario_protocol::BuildFriendStatusNotify(4242ULL, 0);
  ASSERT_EQ(frame.size(), 24U + 24U);
  std::uint64_t symbol = 0;
  std::uint64_t length = 0;
  std::memcpy(&symbol, frame.data() + 8, 8);
  std::memcpy(&length, frame.data() + 16, 8);
  EXPECT_EQ(symbol, 0x26a19dc4d2d5579dULL);
  EXPECT_EQ(length, 24U);
  std::uint64_t id = 0;
  std::uint8_t status = 9;
  ASSERT_TRUE(SocialRoster::ParseStatusNotify(reinterpret_cast<const std::uint8_t*>(frame.data()) + 24,
                                              static_cast<std::size_t>(length), &id, &status));
  EXPECT_EQ(id, 4242ULL);
  EXPECT_EQ(status, 0);

  SocialRoster::Roster roster;
  ASSERT_TRUE(SocialRoster::Feed(roster, "FriendStatusNotify",
                                 reinterpret_cast<const std::uint8_t*>(frame.data()) + 24,
                                 static_cast<std::size_t>(length)));
  EXPECT_TRUE(roster.Contains(4242ULL));
  EXPECT_EQ(roster.Online(), 1U);
}

// pnsrad's UserProviderID must report the provider whose CSymbol64 code is 4, the code SNSUserID
// gives every "OVR-ORG-" id the game builds; otherwise the friend invite handler drops the click.
TEST(ProviderIdentity, UserProviderIdPatchReturnsTheOvrSymbolAndFitsTheSite) {
  using namespace ProviderIdentity;
  const auto code = ReturnConstant(kOvrProviderSymbol);
  const std::array<std::uint8_t, 11> expected = {0x48, 0xB8, 0xF8, 0xF4, 0x9F, 0xA8, 0xB1, 0xD0, 0xE8, 0xC8, 0xC3};
  EXPECT_EQ(code, expected);
  EXPECT_LE(code.size(), kPnsradUserProviderIdExpected.size());
  // The original body ends at the ret; the rest of the site is padding the write may reuse.
  EXPECT_EQ(kPnsradUserProviderIdExpected[7], 0xC3);
  for (std::size_t i = 8; i < kPnsradUserProviderIdExpected.size(); ++i) EXPECT_EQ(kPnsradUserProviderIdExpected[i], 0xCC);
  // "RAD", which pnsrad returned, is not "OVR".
  EXPECT_NE(kOvrProviderSymbol, 0xc8e8d0b1a882e3eeULL);
}

// Slot 37 OpenFriendRequestUI is where the game's add-friend node lands; it must put a friend request
// for exactly that account on the wire, in the layout Nakama reads (sns_friends.go
// SNSFriendInviteRequest: RoutingID, LocalUserUUID, SessionGUID, TargetUserID).
TEST(SocialFriends, AddFriendSendsAFriendRequestForTheTarget) {
  SocialParty::State party;
  party.SetSelf(4242);
  const std::vector<SocialParty::Message> out = party.RequestFriend(5151);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].symbol, 0x7f0d7a28de3c6f70ULL);
  ASSERT_EQ(out[0].payload.size(), 0x28U);
  std::uint64_t target = 0;
  std::memcpy(&target, out[0].payload.data() + 0x20, sizeof(target));
  EXPECT_EQ(target, 5151U);
  EXPECT_STREQ(SocialParty::RequestName(0x7f0d7a28de3c6f70ULL), "FriendInviteRequest");
  EXPECT_TRUE(party.RequestFriend(0).empty());
}

// Every message that changes who is a friend triggers a list refresh; presence and the list itself
// do not (they are what the refresh returns, so treating them as changes would loop).
TEST(SocialFriends, FriendChangesAreRecognisedAndTheRefreshRepliesAreNot) {
  for (const char* name : {"FriendAcceptNotify", "FriendAcceptSuccess", "FriendRemoveNotify", "FriendRemoveResponse",
                           "FriendWithdrawnNotify", "FriendRejectNotify", "FriendInviteNotify", "FriendInviteSuccess",
                           "SNSFriendAcceptNotify"}) {
    EXPECT_TRUE(SocialRoster::IsFriendChange(name)) << name;
  }
  for (const char* name : {"FriendStatusNotify", "FriendListResponse", "PartyInviteNotify", "FriendInviteFailure"}) {
    EXPECT_FALSE(SocialRoster::IsFriendChange(name)) << name;
  }
  EXPECT_FALSE(SocialRoster::IsFriendChange(nullptr));
  EXPECT_TRUE(SocialRoster::IsFriendChangeSymbol(0xc237c84c31d3ae05ULL));   // FriendAcceptNotify
  EXPECT_FALSE(SocialRoster::IsFriendChangeSymbol(0x26a19dc4d2d5579dULL));  // FriendStatusNotify
  EXPECT_FALSE(SocialRoster::IsFriendChangeSymbol(0xa78aeb2a4e89b10bULL));  // FriendListResponse
}

TEST(ScenarioProtocol, InjectsFriendNotifiesAndFiresAddFriend) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"FriendAcceptNotify","id":4242})", &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kInjectFriendNotify);
  EXPECT_EQ(cmd.notifySymbol, 0xc237c84c31d3ae05ULL);
  const std::string frame =
      nevr_scenario_protocol::BuildFriendNotify(*nevr_scenario_protocol::FindFriendNotify("FriendAcceptNotify"), 4242);
  ASSERT_EQ(frame.size(), 24U + 24U);
  std::uint64_t id = 0;
  std::memcpy(&id, frame.data() + 24 + 8, sizeof(id));
  EXPECT_EQ(id, 4242U);
  EXPECT_EQ(nevr_scenario_protocol::BuildFriendNotify(*nevr_scenario_protocol::FindFriendNotify("FriendRemoveNotify"), 4242).size(),
            24U + 16U);
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"fire","action":"add_friend","user":"OVR-ORG-4242"})", &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kFireAddFriend);
}

// An injected party invite is the frame Nakama sends the invitee, and the facade lists it.
TEST(ScenarioProtocol, InjectedPartyInviteReachesTheInviteListAndRespondParses) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyInviteNotify","party":77,"inviter":4242})",
                                             &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kInjectPartyInvite);
  const std::string frame = nevr_scenario_protocol::BuildPartyInviteNotify(77, 4242);
  std::uint64_t symbol = 0;
  std::uint64_t length = 0;
  std::memcpy(&symbol, frame.data() + 8, 8);
  std::memcpy(&length, frame.data() + 16, 8);
  ASSERT_EQ(length, 16U);
  SocialParty::State party;
  party.SetSelf(1);
  std::vector<SocialParty::Message> outgoing;
  ASSERT_TRUE(party.Feed(symbol, reinterpret_cast<const std::uint8_t*>(frame.data()) + 24, 16, 0, &outgoing));
  const SocialParty::View view = party.Snapshot();
  ASSERT_EQ(view.invites.size(), 1U);
  EXPECT_EQ(view.invites[0].partyId, 77U);
  EXPECT_EQ(view.invites[0].senderId, 4242U);

  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"fire","action":"respond_to_invite","index":0,"accept":true})",
                                             &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kFireRespondInvite);
  EXPECT_TRUE(cmd.accept);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"fire","action":"respond_to_invite","index":0})", &cmd, &error));
  EXPECT_NE(error.find("\"accept\""), std::string::npos);
}

TEST(ScenarioProtocol, InjectedMemberJoinAndLeaveChangeTheCurrentParty) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyJoinNotify","member":4242})", &cmd, &error))
      << error;
  ASSERT_EQ(cmd.op, nevr_scenario_protocol::Op::kInjectPartyMember);
  EXPECT_EQ(cmd.partyId, 0U) << "no party given: the current one";
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyLeaveNotify"})", &cmd, &error));
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyJoinSuccess","party":7,"owner":4242})", &cmd,
                                             &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kInjectPartyMember);
  EXPECT_EQ(cmd.memberId, 4242U);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyJoinSuccess","party":7})", &cmd, &error));
  SocialParty::State party;
  party.SetSelf(1);
  ASSERT_TRUE(FeedParty(party, "PartyCreateSuccess", U64s({7, 1})));
  party.DrainEvents();
  const auto feed = [&](const char* name) {
    const std::string frame = nevr_scenario_protocol::BuildPartyMemberNotify(name, 7, 4242);
    std::uint64_t symbol = 0;
    std::memcpy(&symbol, frame.data() + 8, 8);
    EXPECT_STREQ(SocialParty::ReplyName(symbol), name);
    return party.Feed(symbol, reinterpret_cast<const std::uint8_t*>(frame.data()) + 24, frame.size() - 24, 0, nullptr);
  };
  ASSERT_TRUE(feed("PartyJoinNotify"));
  EXPECT_EQ(party.Snapshot().members.size(), 2U);
  ASSERT_TRUE(feed("PartyLeaveNotify"));
  EXPECT_EQ(party.Snapshot().members.size(), 1U);
  const auto events = party.DrainEvents();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kMemberJoined);
  EXPECT_EQ(events[1].kind, SocialParty::EventKind::kMemberLeft);
}

TEST(ScenarioProtocol, EveryFireActionParsesAndBadArgumentsAreNamed) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  const char* good[] = {
      R"({"op":"fire","action":"invite_users","mode":1,"user":"OVR-ORG-4242"})",
      R"({"op":"fire","action":"invite_users","mode":0})",
      R"({"op":"fire","action":"request_profile","user":"self"})",
      R"({"op":"fire","action":"party_join","party":77})",
      R"({"op":"fire","action":"party_lock","lock":true})",
      R"({"op":"fire","action":"set_join_policy","policy":3})",
      R"({"op":"fire","action":"voip_mute_self","mute":false})",
      R"({"op":"fire","action":"voip_mute_user","user":"OVR-ORG-4242","mute":true})",
      R"({"op":"fire","action":"social_groups_set_active","index":0})",
      R"({"op":"fire","action":"social_groups_set_active","index":"current"})",
      R"({"op":"fire","action":"enable_social_feature","feature":1,"enable":true})",
      R"({"op":"fire","action":"set_party_member_string","key":"k","value":"v"})",
      R"({"op":"fire","action":"refresh_recently_met"})",
      R"({"op":"fire","action":"refresh_friends"})",
      R"({"op":"fire","action":"find_arena"})",
      R"({"op":"fire","action":"party_join_failed_callback","code":2})",
  };
  for (const char* line : good) {
    error.clear();
    EXPECT_TRUE(nevr_scenario_protocol::ParseCommand(line, &cmd, &error)) << line << ": " << error;
    EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kFireAction) << line;
  }
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"fire","action":"party_lock","lock":false,"mask":4})", &cmd, &error));
  EXPECT_FALSE(cmd.flag);
  EXPECT_EQ(cmd.number, 4u);
  const char* bad[] = {
      R"({"op":"fire","action":"invite_users","mode":3})",
      R"({"op":"fire","action":"party_join","party":0})",
      R"({"op":"fire","action":"set_join_policy","policy":4})",
      R"({"op":"fire","action":"voip_mute_self"})",
      R"({"op":"fire","action":"set_party_string","key":"","value":"v"})",
      R"({"op":"fire","action":"no_such_node"})",
      R"({"op":"fire","action":"party_join_failed_callback"})",
  };
  for (const char* line : bad) {
    error.clear();
    EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(line, &cmd, &error)) << line;
    EXPECT_FALSE(error.empty()) << line;
  }
}

TEST(ScenarioProtocol, InjectedPartyJoinFailureEndsTheJoinWithTheGamesCode) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyJoinFailure","party":77,"code":2})", &cmd,
                                             &error))
      << error;
  ASSERT_EQ(cmd.op, nevr_scenario_protocol::Op::kInjectPartyJoinFailure);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyJoinFailure","party":77})", &cmd, &error));
  const std::string frame = nevr_scenario_protocol::BuildPartyJoinFailure(77, 2);
  std::uint64_t symbol = 0;
  std::uint64_t length = 0;
  std::memcpy(&symbol, frame.data() + 8, 8);
  std::memcpy(&length, frame.data() + 16, 8);
  ASSERT_EQ(length, 9U);
  EXPECT_STREQ(SocialParty::ReplyName(symbol), "PartyJoinFailure");
  SocialParty::State party;
  party.SetSelf(1);
  ASSERT_TRUE(party.BeginJoin(77));
  ASSERT_EQ(party.Join(77).size(), 1U);
  ASSERT_TRUE(party.Feed(symbol, reinterpret_cast<const std::uint8_t*>(frame.data()) + 24, 9, 0, nullptr));
  EXPECT_FALSE(party.Snapshot().joining);
  const auto events = party.DrainEvents();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kJoinFailed);
  EXPECT_EQ(events[0].code, 4U) << "Nakama's refused (2) is the game's not joinable (4)";
}

// ---------------------------------------------------------------------------------------------
// Party data (docs/design/2026-10-01-social-nakama-proposal.md §3)
// ---------------------------------------------------------------------------------------------
namespace partydata {

std::string Json(const SocialParty::JsonText& text) { return text != nullptr ? *text : std::string(); }

TEST(SocialPartyData, DataForThePartyBeingJoinedIsHeldAndNamesEveryMemberOnSuccess) {
  SocialParty::State party;
  party.SetSelf(100);
  ASSERT_FALSE(party.Join(9).empty());
  EXPECT_EQ(party.ReceiveData(9, 0, R"({"lobbyid":"L"})"), SocialParty::DataOutcome::kHeld);
  EXPECT_EQ(party.ReceiveData(9, 300, R"({"headsettype":3})"), SocialParty::DataOutcome::kHeld);
  EXPECT_EQ(party.ReceiveData(9, 200, R"({"headsettype":2})"), SocialParty::DataOutcome::kHeld);
  EXPECT_EQ(party.ReceiveData(9, 100, R"({"mine":1})"), SocialParty::DataOutcome::kOwnIgnored);
  EXPECT_EQ(party.ReceiveData(5, 200, "{}"), SocialParty::DataOutcome::kOtherParty);
  party.DrainEvents();
  ASSERT_TRUE(FeedParty(party, "PartyJoinSuccess", U64s({9, 200})));
  const SocialParty::View view = party.Snapshot();
  ASSERT_EQ(view.members.size(), 3U) << "the leader and the member only the data named";
  EXPECT_EQ(view.members[1].id, 200U);
  EXPECT_EQ(Json(view.members[1].data), R"({"headsettype":2})");
  EXPECT_EQ(view.members[2].id, 300U);
  EXPECT_EQ(Json(view.members[2].data), R"({"headsettype":3})");
  EXPECT_EQ(Json(view.partyData), R"({"lobbyid":"L"})");
  const auto events = party.DrainEvents();
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].kind, SocialParty::EventKind::kJoined);
  EXPECT_EQ(events[1].kind, SocialParty::EventKind::kMemberJoined);
  EXPECT_EQ(events[1].index, 1U);
  EXPECT_EQ(events[2].kind, SocialParty::EventKind::kMemberJoined);
  EXPECT_EQ(events[2].index, 2U);

  // In the party: an unknown member's data adds them (pnsovr's data packet did), a known one's
  // replaces their data, and a JoinNotify for someone already added changes nothing.
  EXPECT_EQ(party.ReceiveData(9, 400, R"({"headsettype":1})"), SocialParty::DataOutcome::kMemberAdded);
  EXPECT_EQ(party.ReceiveData(9, 200, R"({"headsettype":4})"), SocialParty::DataOutcome::kMember);
  ASSERT_TRUE(FeedParty(party, "PartyJoinNotify", U64s({9, 400})));
  const SocialParty::View after = party.Snapshot();
  ASSERT_EQ(after.members.size(), 4U);
  EXPECT_EQ(Json(after.members[1].data), R"({"headsettype":4})");
  const auto more = party.DrainEvents();
  ASSERT_EQ(more.size(), 1U);
  EXPECT_EQ(more[0].kind, SocialParty::EventKind::kMemberJoined);
  EXPECT_EQ(more[0].index, 3U);
  ASSERT_TRUE(FeedParty(party, "PartyLeaveNotify", U64s({9, 200})));
  EXPECT_EQ(party.Snapshot().members[1].id, 300U) << "the list closes up; the data goes with its member";
  EXPECT_EQ(Json(party.Snapshot().members[1].data), R"({"headsettype":3})");
}

TEST(SocialPartyData, AFailedJoinForgetsTheHeldDataAndTheLeaderIgnoresThePartysOwn) {
  SocialParty::State party;
  party.SetSelf(100);
  ASSERT_FALSE(party.Join(11).empty());
  EXPECT_EQ(party.ReceiveData(11, 200, "{}"), SocialParty::DataOutcome::kHeld);
  ASSERT_TRUE(FeedParty(party, "PartyJoinFailure", U64s({11}) + std::string(1, '\x05')));
  ASSERT_FALSE(party.Join(11).empty());
  ASSERT_TRUE(FeedParty(party, "PartyJoinSuccess", U64s({11, 500})));
  ASSERT_EQ(party.Snapshot().members.size(), 2U) << "only the leader: the failed join's data is gone";
  EXPECT_EQ(party.Snapshot().members[1].data, nullptr);

  SocialParty::State leader;
  leader.SetSelf(100);
  ASSERT_TRUE(FeedParty(leader, "PartyCreateSuccess", U64s({7, 100})));
  EXPECT_EQ(leader.ReceiveData(7, 0, "{}"), SocialParty::DataOutcome::kOwnIgnored);
  EXPECT_EQ(leader.Snapshot().partyData, nullptr);
}

TEST(SocialPartyData, ShareDataIsTheRequestNakamaReadsNumberedPerSend) {
  SocialParty::State party;
  party.SetSelf(100);
  EXPECT_TRUE(party.ShareData(SocialParty::kPartyDataScopeMember, "{}").empty()) << "no party, nothing to share";
  ASSERT_TRUE(FeedParty(party, "PartyJoinSuccess", U64s({9, 200})));
  EXPECT_TRUE(party.ShareData(SocialParty::kPartyDataScopeParty, "{}").empty()) << "only the leader shares the party's";
  const auto first = party.ShareData(SocialParty::kPartyDataScopeMember, R"({"k":"v"})");
  const auto second = party.ShareData(SocialParty::kPartyDataScopeMember, R"({"k":"w"})");
  ASSERT_EQ(first.size(), 1U);
  ASSERT_EQ(second.size(), 1U);
  EXPECT_EQ(first[0].symbol, 0x3448ca6e8d9dd0ceULL);
  EXPECT_STREQ(SocialParty::RequestName(first[0].symbol), "PartyDataUpdateRequest");
  const std::string& p = first[0].payload;
  ASSERT_EQ(p.size(), 0x28U + 8 + 9);
  std::uint64_t scope = 0;
  std::uint32_t seq1 = 0, seq2 = 0, length = 0;
  std::memcpy(&scope, p.data() + 0x20, 8);
  std::memcpy(&seq1, p.data() + 0x28, 4);
  std::memcpy(&length, p.data() + 0x2C, 4);
  std::memcpy(&seq2, second[0].payload.data() + 0x28, 4);
  EXPECT_EQ(scope, 1U);
  EXPECT_EQ(length, 9U);
  EXPECT_EQ(p.substr(0x30), R"({"k":"v"})");
  EXPECT_EQ(seq2, seq1 + 1) << "the server keeps only a newer seq from the same session";
}

TEST(SocialPartyData, TheNotifyFrameRoundTripsAndATruncatedOneIsRefused) {
  const std::string frame = nevr_scenario_protocol::BuildPartyDataNotify(9, 200, 4, R"({"headsettype":2})");
  std::uint64_t symbol = 0;
  std::memcpy(&symbol, frame.data() + 8, 8);
  EXPECT_EQ(symbol, SocialParty::kPartyDataNotify);
  const auto* payload = reinterpret_cast<const std::uint8_t*>(frame.data()) + 24;
  SocialParty::DataNotify notify;
  ASSERT_TRUE(SocialParty::ParseDataNotify(payload, frame.size() - 24, &notify));
  EXPECT_EQ(notify.partyId, 9U);
  EXPECT_EQ(notify.memberId, 200U);
  EXPECT_EQ(notify.seq, 4U);
  EXPECT_EQ(notify.json, R"({"headsettype":2})");
  EXPECT_FALSE(SocialParty::ParseDataNotify(payload, frame.size() - 25, &notify)) << "JsonLen past the end";
  EXPECT_FALSE(SocialParty::ParseDataNotify(payload, 23, &notify));
  EXPECT_EQ(SocialParty::ReplyName(SocialParty::kPartyDataNotify), nullptr) << "the bridge routes it, not Feed";

  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(
      R"({"op":"inject","msg":"PartyDataNotify","member":200,"json":{"headsettype":2}})", &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kInjectPartyData);
  EXPECT_EQ(cmd.memberId, 200U);
  EXPECT_EQ(cmd.value, R"({"headsettype":2})");
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"PartyDataNotify","member":200})", &cmd, &error));
}

// A fake CJson for the facade tests: [+0] holds a heap string with the document's text.
std::string* FakeDoc(void* json) {
  std::string* doc = nullptr;
  std::memcpy(&doc, json, sizeof(doc));
  return doc;
}
void FakeClear(void* json) {
  delete FakeDoc(json);
  std::memset(json, 0, 16);
}
std::uint32_t FakeLoad(void* json, const char* text, std::int64_t length) {
  FakeClear(json);
  auto* doc = new std::string(text, static_cast<std::size_t>(length));
  std::memcpy(json, &doc, sizeof(doc));
  return 0;
}
void* FakeSerialize(void* json, void* block, std::int32_t, const char*) {
  const std::string* doc = FakeDoc(json);
  auto* text = new std::string(doc != nullptr ? *doc : std::string());
  const char* data = text->c_str();
  const std::uint64_t length = text->size();
  std::memcpy(block, &data, sizeof(data));
  std::memcpy(static_cast<std::uint8_t*>(block) + 8, &text, sizeof(text));  // kept for the destroy
  std::memcpy(static_cast<std::uint8_t*>(block) + 0x30, &length, sizeof(length));
  return block;
}
void FakeBlockReset(void*) {}
void FakeBlockDestroy(void* block) {
  std::string* text = nullptr;
  std::memcpy(&text, static_cast<std::uint8_t*>(block) + 8, sizeof(text));
  delete text;
}

std::vector<std::string> g_calls;
std::vector<std::string> g_sent;
void* g_object = nullptr;

std::string SlotText(std::size_t index) {
  std::uint8_t* array = nullptr;
  std::memcpy(&array, static_cast<std::uint8_t*>(g_object) + 0x248, sizeof(array));
  const std::string* doc = FakeDoc(array + 16 * index);
  return doc != nullptr ? *doc : std::string("<empty>");
}
std::string PartyText() {
  const std::string* doc = FakeDoc(static_cast<std::uint8_t*>(g_object) + 0x1F0);
  return doc != nullptr ? *doc : std::string("<empty>");
}
void OnVoid(void* context, void*) { g_calls.push_back(static_cast<const char*>(context) + std::string(" party=") + PartyText()); }
void OnIndex(void* context, void*, std::uint32_t index) {
  g_calls.push_back(static_cast<const char*>(context) + std::string(" ") + std::to_string(index) + " " + SlotText(index));
}
bool CaptureSend(const std::string& frame) {
  g_sent.push_back(frame);
  return true;
}

void Bind(std::array<std::uint8_t, 0x1E0>& table, std::size_t index, const char* name, void* function) {
  std::memcpy(table.data() + 0x20 * index, &name, sizeof(name));
  std::memcpy(table.data() + 0x20 * index + 0x18, &function, sizeof(function));
}

TEST(SocialFacadeData, ReceivedDataIsInTheGamesJsonBeforeMemberJoinedAndLocalWritesAreShared) {
  using UpdateFn = void (*)(void*, const void*);
  using InitializeFn = std::uint64_t (*)(void*, std::uint32_t, const void*);
  using WritableFn = std::uint64_t (*)(void*, std::int32_t);
  using ShutdownFn = void (*)(void*);
  SocialFacade::JsonOps ops;
  ops.load = &FakeLoad;
  ops.clear = &FakeClear;
  ops.serialize = &FakeSerialize;
  ops.blockReset = &FakeBlockReset;
  ops.blockDestroy = &FakeBlockDestroy;
  SocialFacade::SetJsonOps(ops);
  SocialParty::SetSender(&CaptureSend);
  g_object = SocialFacade::Object();
  const Slot* vtable = Vtable(g_object);
  std::array<std::uint8_t, 0x1E0> table{};
  Bind(table, 1, "Joined", reinterpret_cast<void*>(&OnVoid));
  Bind(table, 3, "Updated", reinterpret_cast<void*>(&OnVoid));
  Bind(table, 9, "MemberJoined", reinterpret_cast<void*>(&OnIndex));
  Bind(table, 10, "MemberUpdated", reinterpret_cast<void*>(&OnIndex));
  reinterpret_cast<InitializeFn>(vtable[9])(g_object, 1, table.data());
  SocialParty::State& party = SocialParty::Global();
  party.SetSelf(100, "Me");
  party.ResetParty();
  std::uint8_t flags = 0;
  const auto update = [&] { reinterpret_cast<UpdateFn>(vtable[13])(g_object, &flags); };
  update();
  party.DrainEvents();
  g_calls.clear();
  g_sent.clear();

  ASSERT_FALSE(party.Join(9).empty());
  party.ReceiveData(9, 0, R"({"lobbyid":"L"})");
  party.ReceiveData(9, 200, R"({"headsettype":2})");
  ASSERT_TRUE(FeedParty(party, "PartyJoinSuccess", U64s({9, 200})));
  update();
  ASSERT_GE(g_calls.size(), 4U);
  EXPECT_EQ(g_calls[0], R"(Joined party={"lobbyid":"L"})") << "the party's data was loaded before the events";
  EXPECT_EQ(g_calls[1], R"(MemberJoined 1 {"headsettype":2})") << "PartyMemberJoinedCB finds the headset";
  EXPECT_EQ(g_calls[2], R"(MemberUpdated 1 {"headsettype":2})");
  EXPECT_EQ(g_calls[3], R"(Updated party={"lobbyid":"L"})");
  ASSERT_EQ(g_sent.size(), 1U) << "entering the party shares the local member's data once";
  EXPECT_EQ(g_sent[0].substr(24 + 0x30), "{}");

  // A data update fires MemberUpdated for that member only.
  g_calls.clear();
  party.ReceiveData(9, 200, R"({"headsettype":4})");
  update();
  ASSERT_EQ(g_calls.size(), 1U);
  EXPECT_EQ(g_calls[0], R"(MemberUpdated 1 {"headsettype":4})");

  // The game writes its member data through slot 30; the next Update shares it.
  g_sent.clear();
  const std::uint64_t root = reinterpret_cast<WritableFn>(vtable[30])(g_object, 0);
  ASSERT_NE(root, 0U);
  FakeLoad(reinterpret_cast<void*>(static_cast<std::uintptr_t>(root)), R"({"k":"v"})", 9);
  update();
  ASSERT_EQ(g_sent.size(), 1U);
  EXPECT_EQ(g_sent[0].substr(24 + 0x30), R"({"k":"v"})");
  update();
  EXPECT_EQ(g_sent.size(), 1U) << "nothing new written, nothing sent";

  // The member leaves: their slot is cleared. Reset releases everything.
  ASSERT_TRUE(FeedParty(party, "PartyLeaveNotify", U64s({9, 200})));
  update();
  EXPECT_EQ(SlotText(1), "<empty>");
  using ResetFn = void (*)(void*);
  reinterpret_cast<ResetFn>(vtable[12])(g_object);
  EXPECT_EQ(SlotText(0), "<empty>");
  EXPECT_EQ(PartyText(), "<empty>");

  party.DrainEvents();
  reinterpret_cast<ShutdownFn>(vtable[10])(g_object);
  SocialParty::SetSender(nullptr);
  SocialFacade::SetJsonOps(SocialFacade::JsonOps{});
}

TEST(SocialFacadeData, TheLeadersWrittenPartyDataIsSharedAndTheWrittenBitCleared) {
  using UpdateFn = void (*)(void*, const void*);
  SocialFacade::JsonOps ops;
  ops.load = &FakeLoad;
  ops.clear = &FakeClear;
  ops.serialize = &FakeSerialize;
  ops.blockReset = &FakeBlockReset;
  ops.blockDestroy = &FakeBlockDestroy;
  SocialFacade::SetJsonOps(ops);
  SocialParty::SetSender(&CaptureSend);
  g_object = SocialFacade::Object();
  const Slot* vtable = Vtable(g_object);
  SocialParty::State& party = SocialParty::Global();
  party.SetSelf(100, "Me");
  party.ResetParty();
  std::uint8_t flags = 0;
  const auto update = [&] { reinterpret_cast<UpdateFn>(vtable[13])(g_object, &flags); };
  ASSERT_TRUE(FeedParty(party, "PartyCreateSuccess", U64s({7, 100})));
  update();
  g_sent.clear();
  FakeLoad(static_cast<std::uint8_t*>(g_object) + 0x1F0, R"({"p":1})", 7);
  std::uint32_t word = 0;
  std::memcpy(&word, static_cast<std::uint8_t*>(g_object) + 0x27C, 4);
  word |= 1U;  // what 0x14015fdb0 does when the host writes party data
  std::memcpy(static_cast<std::uint8_t*>(g_object) + 0x27C, &word, 4);
  update();
  ASSERT_EQ(g_sent.size(), 1U);
  std::uint64_t scope = 1;
  std::memcpy(&scope, g_sent[0].data() + 24 + 0x20, 8);
  EXPECT_EQ(scope, 0U);
  EXPECT_EQ(g_sent[0].substr(24 + 0x30), R"({"p":1})");
  std::memcpy(&word, static_cast<std::uint8_t*>(g_object) + 0x27C, 4);
  EXPECT_EQ(word & 1U, 0U) << "shared, so no longer marked written";
  update();
  EXPECT_EQ(g_sent.size(), 1U);

  party.ResetParty();
  party.DrainEvents();
  using ResetFn = void (*)(void*);
  reinterpret_cast<ResetFn>(vtable[12])(g_object);
  SocialParty::SetSender(nullptr);
  SocialFacade::SetJsonOps(SocialFacade::JsonOps{});
}

}  // namespace partydata

// ---------------------------------------------------------------------------------------------
// Recently met (docs/design/2026-10-01-social-nakama-proposal.md §2)
// ---------------------------------------------------------------------------------------------
namespace recentlymet {

std::vector<SocialRoster::Entry> TwoPeople() {
  SocialRoster::Entry off;
  off.id = 42;
  off.name = "Off";
  SocialRoster::Entry on;
  on.id = 900000000000000101ULL;
  on.name = "Peer";
  on.online = true;
  on.presence.partyId = 5;
  on.presence.joinable = true;
  on.presence.text = "Social Lobby";
  return {off, on};  // offline first on purpose: the list shows online people first
}

TEST(SocialRecentlyMet, TheResponseRoundTripsAndATruncatedOneIsRefused) {
  const std::string frame = nevr_scenario_protocol::BuildRecentlyMetListResponse(TwoPeople());
  std::uint64_t symbol = 0;
  std::memcpy(&symbol, frame.data() + 8, 8);
  EXPECT_EQ(symbol, 0xbc3ee692bb03328fULL);
  const auto* payload = reinterpret_cast<const std::uint8_t*>(frame.data()) + 24;
  std::vector<SocialRoster::Entry> people;
  ASSERT_TRUE(SocialRoster::ParseRecentlyMetResponse(payload, frame.size() - 24, &people));
  ASSERT_EQ(people.size(), 2U);
  EXPECT_EQ(people[0].id, 42U);
  EXPECT_FALSE(people[0].online);
  EXPECT_EQ(people[1].name, "Peer");
  EXPECT_EQ(people[1].presence.text, "Social Lobby");
  EXPECT_EQ(people[1].presence.partyId, 5U);
  EXPECT_FALSE(SocialRoster::ParseRecentlyMetResponse(payload, frame.size() - 25, &people));
  const std::uint8_t empty[4] = {0, 0, 0, 0};
  ASSERT_TRUE(SocialRoster::ParseRecentlyMetResponse(empty, 4, &people));
  EXPECT_TRUE(people.empty());
}

TEST(SocialRecentlyMet, ARefreshIsBusyUntilItsAnswerOrFiveSecondsAndShowsOnlinePeopleFirst) {
  SocialRoster::RecentList list;
  EXPECT_FALSE(list.Refreshing(100));
  ASSERT_TRUE(list.BeginRefresh(100));
  EXPECT_FALSE(list.BeginRefresh(101)) << "one in flight";
  EXPECT_TRUE(list.Refreshing(104));
  list.SetList(TwoPeople());
  EXPECT_FALSE(list.Refreshing(104)) << "the answer ends it";
  ASSERT_EQ(list.Count(), 2U);
  EXPECT_EQ(list.Online(), 1U);
  std::uint64_t id = 0;
  ASSERT_TRUE(list.IdAt(0, &id));
  EXPECT_EQ(id, 900000000000000101ULL);
  EXPECT_STREQ(list.NameAt(1), "Off");
  EXPECT_EQ(list.PartyIdAt(0), 5U);
  EXPECT_EQ(list.PartyIdAt(1), 0U);

  ASSERT_TRUE(list.BeginRefresh(200));
  bool timedOut = false;
  EXPECT_TRUE(list.Refreshing(204, &timedOut));
  EXPECT_FALSE(timedOut);
  EXPECT_FALSE(list.Refreshing(205, &timedOut)) << "a server that never answers does not hang the node";
  EXPECT_TRUE(timedOut);
  EXPECT_EQ(list.Count(), 2U) << "the list stays as it was";
}

std::vector<std::string> g_frames;
bool Capture(const std::string& frame) {
  g_frames.push_back(frame);
  return true;
}

TEST(SocialFacadeRecentlyMet, SlotsAnswerFromTheServersList) {
  using U32Fn = std::uint32_t (*)(void*);
  using U64Fn = std::uint64_t (*)(void*);
  using VoidFn = void (*)(void*);
  using IdFn = std::uint64_t* (*)(void*, std::uint64_t*, std::uint32_t);
  using TextFn = const char* (*)(void*, std::int32_t);
  using IndexFn = std::uint32_t (*)(void*, std::uint32_t);
  using PartyFn = std::uint64_t (*)(void*, std::uint32_t);
  void* object = SocialFacade::Object();
  const Slot* vtable = Vtable(object);
  SocialParty::Global().SetSelf(100, "Me");
  SocialParty::SetSender(&Capture);
  g_frames.clear();

  reinterpret_cast<VoidFn>(vtable[57])(object);
  ASSERT_EQ(g_frames.size(), 1U);
  std::uint64_t symbol = 0;
  std::memcpy(&symbol, g_frames[0].data() + 8, 8);
  EXPECT_EQ(symbol, 0xc5359d9ff7e1fefeULL);
  EXPECT_EQ(g_frames[0].size(), 24U + 0x20) << "the 0x20 header";
  EXPECT_EQ(reinterpret_cast<U64Fn>(vtable[56])(object), 1U) << "refreshing until the answer";
  reinterpret_cast<VoidFn>(vtable[57])(object);
  EXPECT_EQ(g_frames.size(), 1U) << "no second request while one is in flight";

  // A party of one that is joinable, so an online person who is not a member can be invited.
  ASSERT_TRUE(FeedParty(SocialParty::Global(), "PartyCreateSuccess", U64s({7, 100})));
  SocialRoster::RecentlyMet().SetList(TwoPeople());
  std::uint8_t flags = 0;
  reinterpret_cast<void (*)(void*, const void*)>(vtable[13])(object, &flags);
  EXPECT_EQ(reinterpret_cast<U64Fn>(vtable[56])(object), 0U);
  EXPECT_EQ(reinterpret_cast<U32Fn>(vtable[58])(object), 2U);
  EXPECT_EQ(reinterpret_cast<U32Fn>(vtable[59])(object), 1U);
  EXPECT_EQ(reinterpret_cast<U32Fn>(vtable[60])(object), 1U);
  std::uint64_t id = 0;
  reinterpret_cast<IdFn>(vtable[61])(object, &id, 0);
  EXPECT_EQ(id, 900000000000000101ULL);
  EXPECT_STREQ(reinterpret_cast<TextFn>(vtable[62])(object, 0), "Peer");
  EXPECT_STREQ(reinterpret_cast<TextFn>(vtable[62])(object, -1), "");
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[63])(object, 0), 2U);
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[63])(object, 1), 0U);
  EXPECT_STREQ(reinterpret_cast<TextFn>(vtable[64])(object, 0), "Social Lobby");
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[65])(object, 0), 1U) << "online, not a member, party joinable";
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[65])(object, 1), 0U) << "offline";
  EXPECT_EQ(reinterpret_cast<IndexFn>(vtable[66])(object, 0), 1U);
  EXPECT_EQ(reinterpret_cast<PartyFn>(vtable[67])(object, 0), 5U);
  EXPECT_EQ(reinterpret_cast<PartyFn>(vtable[67])(object, 9), 0U);

  SocialRoster::RecentlyMet().SetList({});
  SocialParty::Global().ResetParty();
  SocialParty::Global().DrainEvents();
  SocialParty::SetSender(nullptr);
}

TEST(ScenarioProtocol, RecentlyMetInjectParses) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(nevr_scenario_protocol::ParseCommand(
      R"({"op":"inject","msg":"RecentlyMetListResponse","users":[{"id":7,"name":"A","online":true,"party":3,"text":"In Main Menu"},{"id":8}]})",
      &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kInjectRecentlyMet);
  ASSERT_EQ(cmd.people.size(), 2U);
  EXPECT_TRUE(cmd.people[0].presence.joinable);
  EXPECT_FALSE(cmd.people[1].online);
  EXPECT_FALSE(nevr_scenario_protocol::ParseCommand(R"({"op":"inject","msg":"RecentlyMetListResponse"})", &cmd, &error));
}

}  // namespace recentlymet
