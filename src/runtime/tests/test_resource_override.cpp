#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

#include "runtime/patch/resource_override.h"

namespace {

using namespace std::chrono_literals;

std::mutex g_gateMutex;
std::condition_variable g_gate;
bool g_originalEntered = false;
bool g_releaseOriginal = false;
const void* g_forwardedBuffer = nullptr;
constexpr std::uint64_t kType = 0x11223344;
constexpr std::uint64_t kName = 0x55667788;
constexpr char kReentrantBytes[] = "reentrant";

void BlockingOriginal(void* callbackData) {
  // A game callback may call back into the exported registry API on this thread.
  RegisterResourceOverride(kType, kName + 1, kReentrantBytes, sizeof(kReentrantBytes), "reentrant");
  const auto* bytes = static_cast<const std::uint8_t*>(callbackData);
  const void* forwarded = nullptr;
  std::memcpy(&forwarded, bytes + 0x18, sizeof(forwarded));
  {
    std::lock_guard<std::mutex> lock(g_gateMutex);
    g_forwardedBuffer = forwarded;
    g_originalEntered = true;
  }
  g_gate.notify_all();
  std::unique_lock<std::mutex> lock(g_gateMutex);
  g_gate.wait(lock, [] { return g_releaseOriginal; });
}

void SelfDeregisteringOriginal(void* callbackData) {
  const auto* bytes = static_cast<const std::uint8_t*>(callbackData);
  const void* forwarded = nullptr;
  std::uint64_t size = 0;
  std::memcpy(&forwarded, bytes + 0x18, sizeof(forwarded));
  std::memcpy(&size, bytes + 0x20, sizeof(size));
  const auto* start = static_cast<const std::uint8_t*>(forwarded);
  DeregisterResourceOverrides(start, start + size);
}

void RunSelfDeregisterFixture() {
  TestHook_InitResourceOverrides(&SelfDeregisteringOriginal);
  char replacement[] = "replacement";
  RegisterResourceOverride(kType, kName, replacement, sizeof(replacement), "test");

  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x28> callback{};
  void* resourcePtr = resource.data();
  std::memcpy(resource.data() + 0x28, &kType, sizeof(kType));
  std::memcpy(resource.data() + 0x38, &kName, sizeof(kName));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));
  TestHook_RunResourceCallback(callback.data());
}

TEST(ResourceOverride, DeregistrationWaitsForOriginalCallbackAndReentryWorks) {
  {
    std::lock_guard<std::mutex> lock(g_gateMutex);
    g_originalEntered = false;
    g_releaseOriginal = false;
    g_forwardedBuffer = nullptr;
  }
  TestHook_InitResourceOverrides(&BlockingOriginal);
  char replacement[] = "replacement";
  RegisterResourceOverride(kType, kName, replacement, sizeof(replacement), "test");

  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x28> callback{};
  void* resourcePtr = resource.data();
  std::memcpy(resource.data() + 0x28, &kType, sizeof(kType));
  std::memcpy(resource.data() + 0x38, &kName, sizeof(kName));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));

  std::thread callbackThread([&] { TestHook_RunResourceCallback(callback.data()); });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(g_gateMutex);
    entered = g_gate.wait_for(lock, 5s, [] { return g_originalEntered; });
  }
  if (!entered) {
    {
      std::lock_guard<std::mutex> lock(g_gateMutex);
      g_releaseOriginal = true;
    }
    g_gate.notify_all();
    callbackThread.join();
    TestHook_CloseResourceOverrides();
    FAIL() << "original callback could not reenter the registry";
  }

  auto deregister = std::async(std::launch::async, [&] {
    DeregisterResourceOverrides(replacement, replacement + sizeof(replacement));
  });
  const auto whileOriginalRuns = deregister.wait_for(150ms);
  {
    std::lock_guard<std::mutex> lock(g_gateMutex);
    g_releaseOriginal = true;
  }
  g_gate.notify_all();
  callbackThread.join();
  const auto afterOriginalReturns = deregister.wait_for(5s);
  EXPECT_EQ(whileOriginalRuns, std::future_status::timeout)
      << "deregistration erased the override while the game callback still held its buffer";
  EXPECT_EQ(afterOriginalReturns, std::future_status::ready);
  EXPECT_EQ(g_forwardedBuffer, replacement);
  TestHook_CloseResourceOverrides();
}

TEST(ResourceOverride, ReentrantDeregistrationFailsClosed) {
  EXPECT_EXIT(RunSelfDeregisterFixture(),
              ::testing::ExitedWithCode(static_cast<int>(0xE0000478u)), ".*");
}

void StderrSink(const char* line) {
  std::fputs("SINK: ", stderr);
  std::fputs(line, stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void RunSelfDeregisterFixtureWithSink() {
  SetResourceOverrideFatalLineSink(&StderrSink);
  RunSelfDeregisterFixture();
}

// The fail-closed exit used to leave only an exit code. It now writes one record through the sink first.
TEST(ResourceOverride, ReentrantDeregistrationLeavesARecordBeforeItTerminates) {
  EXPECT_EXIT(RunSelfDeregisterFixtureWithSink(),
              ::testing::ExitedWithCode(static_cast<int>(0xE0000478u)),
              "SINK: \\[NEVR\\.RESOURCE\\] FATAL reentrant deregistration: label=test .*0xE0000478");
}

void NoopOriginal(void*) {}

void RunCallbackFor(std::uint64_t name) {
  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x28> callback{};
  void* resourcePtr = resource.data();
  std::memcpy(resource.data() + 0x28, &kType, sizeof(kType));
  std::memcpy(resource.data() + 0x38, &name, sizeof(name));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));
  TestHook_RunResourceCallback(callback.data());
}

TEST(ResourceOverride, ShutdownReportsEveryOverrideTheGameNeverRequested) {
  TestHook_InitResourceOverrides(&NoopOriginal);
  static char first[] = "first";
  static char second[] = "second";
  RegisterResourceOverride(kType, kName, first, sizeof(first), "first");
  RegisterResourceOverride(kType, kName + 2, second, sizeof(second), "second");
  RunCallbackFor(kName);  // only "first" is ever requested

  const ResourceOverrideShutdownReport report = ShutdownResourceOverride();
  EXPECT_EQ(report.registered, 2u);
  EXPECT_EQ(report.never_applied, 1u) << "the never-requested override must be counted";
  EXPECT_TRUE(report.hook_installed) << "with the hook live the registry is kept (module pinned)";
  TestHook_CloseResourceOverrides();
}

TEST(ResourceOverride, ShutdownWithNoHookReleasesTheRegistry) {
  TestHook_InitResourceOverrides(&NoopOriginal);
  static char only[] = "only";
  RegisterResourceOverride(kType, kName, only, sizeof(only), "only");
  TestHook_SetOriginal(nullptr);  // the hook never installed

  const ResourceOverrideShutdownReport report = ShutdownResourceOverride();
  EXPECT_EQ(report.registered, 1u);
  EXPECT_EQ(report.never_applied, 1u);
  EXPECT_FALSE(report.hook_installed);
  // The registry was released: a second shutdown has nothing to report.
  EXPECT_EQ(ShutdownResourceOverride().registered, 0u);
}

}  // namespace
