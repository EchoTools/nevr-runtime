#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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

}  // namespace
