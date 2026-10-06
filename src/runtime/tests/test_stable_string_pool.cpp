#include "runtime/lifecycle/stable_string_pool.h"
#include "quest/tests/stable_string_pool_vectors.h"
#include "runtime/lifecycle/service_map.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

using nevr_runtime::lifecycle::InternStatus;
using nevr_runtime::lifecycle::StableStringPool;
using nevr_runtime::lifecycle::StableStringPoolLimits;

void InjectBadAllocation(void* context) {
  if (*static_cast<bool*>(context)) throw std::bad_alloc();
}

bool IsReadableAddress(const void* address) {
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) return false;
  constexpr DWORD kUnreadable = PAGE_NOACCESS | PAGE_GUARD;
  return (info.Protect & kUnreadable) == 0;
}

std::string AdjacentDllPath(const char* dllName) {
  char executable[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, executable, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return {};
  std::string path(executable, length);
  const std::size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos) return {};
  path.resize(slash + 1);
  path += dllName;
  return path;
}

template <typename Function>
Function LoadExport(HMODULE module, const char* name) {
  return reinterpret_cast<Function>(GetProcAddress(module, name));
}

}  // namespace

TEST(StableStringPool, SharedVectorsDeduplicateAndRemainStableAfterLaterInsertions) {
  StableStringPool pool;
  std::vector<const char*> published;
  for (const auto& vector : nevr_quest_test::kStableStringVectors) {
    const auto result = pool.Intern(vector.value);
    ASSERT_EQ(result.status, InternStatus::kSuccess);
    ASSERT_NE(result.pointer, nullptr);
    EXPECT_EQ(std::strlen(result.pointer) + 1, vector.byteCountWithTerminator);
    published.push_back(result.pointer);
    EXPECT_EQ(pool.Intern(vector.value).pointer, result.pointer);
  }
  EXPECT_STREQ(published[0], "");
  EXPECT_STREQ(published[1], "alpha");
  EXPECT_STREQ(published[2], "beta");
}

TEST(StableStringPool, GeneratedJsonEscapingFitsExactAndRejectsOverLimitPayload) {
  constexpr std::size_t kLimit = nevr_runtime::lifecycle::kStableStringMaxPayloadBytes;
  const std::string httpUri = "https://game.example:7350";
  const auto baseline = nevr_cfg::BuildGameNativeConfigJson(httpUri, "x");
  ASSERT_TRUE(baseline.has_value());
  ASSERT_EQ(baseline->size(), 221u);
  const auto publicFixture = nevr_cfg::BuildGameNativeConfigJson(httpUri, "the-server-key");
  ASSERT_TRUE(publicFixture.has_value());
  ASSERT_EQ(publicFixture->size(), 234u);
  EXPECT_EQ(nlohmann::json::parse(*publicFixture)["social_plugin"]["server_key"], "the-server-key");
  const std::size_t fixedJsonBytes = baseline->size() - 1;
  const std::size_t escapedPayloadBytes = kLimit - fixedJsonBytes;
  ASSERT_EQ(escapedPayloadBytes % 6, 0u);

  // Each NUL expands to the six-byte JSON escape \u0000. The JSON stays valid
  // and contains no literal NUL, while its parsed value retains the input NULs.
  const std::string secretKey(escapedPayloadBytes / 6, '\0');
  const auto exactJson = nevr_cfg::BuildGameNativeConfigJson(httpUri, secretKey);
  ASSERT_TRUE(exactJson.has_value());
  ASSERT_EQ(exactJson->size(), kLimit);

  const nlohmann::json parsed = nlohmann::json::parse(*exactJson);
  const std::string parsedKey = parsed["social_plugin"]["server_key"].get<std::string>();
  EXPECT_EQ(parsedKey.size(), secretKey.size());
  EXPECT_TRUE(parsedKey == secretKey);

  StableStringPool pool;
  const auto accepted = pool.Intern(*exactJson);
  ASSERT_EQ(accepted.status, InternStatus::kSuccess);
  EXPECT_EQ(accepted.liveBytes, kLimit + 1);
  EXPECT_STREQ(accepted.pointer, exactJson->c_str());

  const auto overLimitJson = nevr_cfg::BuildGameNativeConfigJson(httpUri, secretKey + "x");
  ASSERT_TRUE(overLimitJson.has_value());
  ASSERT_EQ(overLimitJson->size(), kLimit + 1);
  const auto rejected = pool.Intern(*overLimitJson);
  EXPECT_EQ(rejected.status, InternStatus::kStringTooLong);
  EXPECT_EQ(rejected.pointer, nullptr);
  EXPECT_EQ(rejected.stringCount, 1u);
  EXPECT_EQ(rejected.liveBytes, kLimit + 1);

  const std::string escapedKey = "a\"b\\c";
  const auto escapedJson = nevr_cfg::BuildGameNativeConfigJson(httpUri, escapedKey);
  ASSERT_TRUE(escapedJson.has_value());
  EXPECT_EQ(escapedJson->size(), 227u);
  EXPECT_EQ(nlohmann::json::parse(*escapedJson)["social_plugin"]["server_key"], escapedKey);
}

TEST(StableStringPool, RejectsEmbeddedNulWithoutChangingAccounting) {
  StableStringPool pool;
  const char embedded[] = {'s', 'e', 'c', 'r', 'e', 't', '\0', 'x'};
  const auto result = pool.Intern(std::string_view(embedded, sizeof(embedded)));
  EXPECT_EQ(result.status, InternStatus::kEmbeddedNul);
  EXPECT_EQ(result.pointer, nullptr);
  EXPECT_EQ(result.stringCount, 0u);
  EXPECT_EQ(result.liveBytes, 0u);
}

TEST(StableStringPool, EnforcesStringCountAndPayloadLimit) {
  StableStringPool pool(StableStringPoolLimits{1, 3, 4});
  const auto exact = pool.Intern("abc");
  ASSERT_EQ(exact.status, InternStatus::kSuccess);
  EXPECT_EQ(exact.liveBytes, 4u);  // three payload bytes plus NUL
  EXPECT_EQ(pool.Intern("abc").pointer, exact.pointer);  // duplicates use no quota

  const auto tooLong = pool.Intern("abcd");
  EXPECT_EQ(tooLong.status, InternStatus::kStringTooLong);
  const auto tooMany = pool.Intern("x");
  EXPECT_EQ(tooMany.status, InternStatus::kPoolCountExceeded);
  EXPECT_EQ(pool.Intern("abc").pointer, exact.pointer);
}

TEST(StableStringPool, EnforcesAggregateBytesIncludingEveryTerminator) {
  StableStringPool pool(StableStringPoolLimits{4, 8, 4});
  const auto first = pool.Intern("ab");
  ASSERT_EQ(first.status, InternStatus::kSuccess);
  EXPECT_EQ(first.liveBytes, 3u);
  EXPECT_EQ(pool.Intern("x").status, InternStatus::kPoolBytesExceeded);
  EXPECT_EQ(pool.Intern("ab").pointer, first.pointer);
  EXPECT_EQ(pool.Intern("ab").liveBytes, 3u);
}

TEST(StableStringPool, AllocationFailureDoesNotPublishOrConsumeQuota) {
  bool failAllocation = true;
  StableStringPool pool(StableStringPoolLimits{2, 32, 64}, InjectBadAllocation, &failAllocation);
  const auto denied = pool.Intern("unpublished");
  EXPECT_EQ(denied.status, InternStatus::kAllocationFailure);
  EXPECT_EQ(denied.pointer, nullptr);
  EXPECT_EQ(denied.stringCount, 0u);
  EXPECT_EQ(denied.liveBytes, 0u);

  failAllocation = false;
  const auto accepted = pool.Intern("unpublished");
  ASSERT_EQ(accepted.status, InternStatus::kSuccess);
  EXPECT_STREQ(accepted.pointer, "unpublished");
  EXPECT_EQ(accepted.stringCount, 1u);
}

TEST(StableStringPool, ConcurrentSameValueReturnsOneStablePointer) {
  StableStringPool pool;
  constexpr std::size_t kThreads = 16;
  std::atomic<bool> start{false};
  std::vector<const char*> results(kThreads, nullptr);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      results[i] = pool.Intern("concurrent-value").pointer;
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread& thread : threads) thread.join();
  ASSERT_NE(results[0], nullptr);
  for (const char* result : results) EXPECT_EQ(result, results[0]);
  EXPECT_STREQ(results[0], "concurrent-value");
}

TEST(StableStringPool, PublishedPointerSurvivesActualFixtureDllUnload) {
  constexpr const char* kDllName = "test_stable_string_pool_fixture.dll";
  const std::string path = AdjacentDllPath(kDllName);
  ASSERT_FALSE(path.empty());
  HMODULE module = LoadLibraryA(path.c_str());
  ASSERT_NE(module, nullptr);
  const auto observeDetach =
      LoadExport<void (*)(bool*)>(module, "StableStringPoolFixtureObserveDetach");
  ASSERT_NE(observeDetach, nullptr);
  bool detached = false;
  observeDetach(&detached);
  const auto intern = LoadExport<const char* (*)(const char*)>(module, "StableStringPoolFixtureIntern");
  ASSERT_NE(intern, nullptr);
  const std::string value = "stable-after-unload";
  const char* pointer = intern(value.c_str());
  ASSERT_NE(pointer, nullptr);
  ASSERT_EQ(GetModuleHandleA(kDllName), module);

  ASSERT_NE(FreeLibrary(module), 0);
  EXPECT_TRUE(detached) << "FreeLibrary did not run the fixture's DLL_PROCESS_DETACH handler";
  EXPECT_EQ(GetModuleHandleA(kDllName), nullptr) << "fixture DLL still has a loaded reference";
  ASSERT_TRUE(IsReadableAddress(pointer));
  EXPECT_STREQ(pointer, value.c_str());
}

TEST(StableStringPool, DestructibleOwnerRedControlSignalsInvalidationOnDllUnload) {
  constexpr const char* kDllName = "test_stable_string_pool_destructible_control.dll";
  const std::string path = AdjacentDllPath(kDllName);
  ASSERT_FALSE(path.empty());
  HMODULE module = LoadLibraryA(path.c_str());
  ASSERT_NE(module, nullptr);
  const auto getPointer = LoadExport<const char* (*)(bool*)>(module, "DestructibleControlPointer");
  ASSERT_NE(getPointer, nullptr);
  bool ownerDestroyed = false;
  const char* controlPointer = getPointer(&ownerDestroyed);
  ASSERT_NE(controlPointer, nullptr);
  EXPECT_STREQ(controlPointer, "destructible-owner-red-control");
  ASSERT_NE(FreeLibrary(module), 0);
  EXPECT_EQ(GetModuleHandleA(kDllName), nullptr) << "red-control DLL still has a loaded reference";

  // This red control is rejected by observing the actual owner destructor;
  // the test never dereferences the invalid pointer after its owner is gone.
  EXPECT_TRUE(ownerDestroyed);
}
