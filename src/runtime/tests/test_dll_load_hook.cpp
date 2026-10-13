// #361: the Oculus Platform SDK block installed its own LoadLibraryW/ExW detours on targets
// DllLoadHook had already hooked, so it failed with MH_ERROR_ALREADY_CREATED on every run. It is a
// load filter on DllLoadHook now; this runs the real hooks and checks the filter vetoes a load.

#include "runtime/hook/dll_load_hook.h"

#include <MinHook.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <cwchar>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

bool BlocksVersionDll(const wchar_t* lowerPath) { return std::wcsstr(lowerPath, L"version.dll") != nullptr; }

bool NeverBlocks(const wchar_t*) { return false; }

// Another thread registers a filter, which needs the registry mutex. If the thread running a
// filter predicate or a patch callback still holds that mutex, this thread cannot finish, and the
// wait below times out. That is a gtest failure instead of a hang. The helper threads are joined
// by JoinLockProbes() once the hook call has returned and released the mutex.
std::mutex g_lockProbesMutex;
std::vector<std::thread> g_lockProbes;

bool RegistryLockIsFreeFromAnotherThread() {
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> finished = done->get_future();
  {
    std::lock_guard<std::mutex> lock(g_lockProbesMutex);
    g_lockProbes.emplace_back([done] {
      nevr_dll_load_hook::AddLoadFilter("lock-probe", NeverBlocks);
      done->set_value();
    });
  }
  return finished.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
}

void JoinLockProbes() {
  std::vector<std::thread> probes;
  {
    std::lock_guard<std::mutex> lock(g_lockProbesMutex);
    probes.swap(g_lockProbes);
  }
  for (auto& probe : probes) probe.join();
}

std::atomic<bool> g_addedFilter{false};
std::atomic<bool> g_predicateHeldLock{false};
bool AddsFilterDuringPredicate(const wchar_t*) {
  if (!g_addedFilter.exchange(true)) {
    // Registering from this thread would deadlock on a held mutex, so check from another thread
    // first and skip the registration when the lock is held: the test then fails by assertion.
    if (!RegistryLockIsFreeFromAnotherThread()) {
      g_predicateHeldLock = true;
      return false;
    }
    nevr_dll_load_hook::AddLoadFilter("added-filter", BlocksVersionDll);
  }
  return false;
}

std::atomic<int> g_firstCalls{0};
std::atomic<int> g_nestedCalls{0};
std::atomic<bool> g_callbackHeldLock{false};
void NestedCallback(const char*, HMODULE) { ++g_nestedCalls; }
void RegisterDuringCallback(const char*, HMODULE module) {
  ++g_firstCalls;
  if (!RegistryLockIsFreeFromAnotherThread()) {
    g_callbackHeldLock = true;
    return;
  }
  nevr_dll_load_hook::OnLoad("nevr-reentrant-unloaded.dll", NestedCallback);
  nevr_dll_load_hook::FireCallbacksForModule("nevr-reentrant-unloaded.dll", module);
}

std::mutex g_probeMutex;
std::condition_variable g_probeCondition;
bool g_probeEntered = false;
bool g_probeRelease = false;
std::atomic<int> g_immediateCalls{0};
std::atomic<int> g_laterCalls{0};
void ImmediateCallback(const char*, HMODULE) { ++g_immediateCalls; }
void LaterCallback(const char*, HMODULE) { ++g_laterCalls; }
HMODULE WINAPI BlockingModuleProbe(LPCSTR name) {
  if (std::strcmp(name, "nevr-loaded-a.dll") != 0) return nullptr;
  std::unique_lock<std::mutex> lock(g_probeMutex);
  g_probeEntered = true;
  g_probeCondition.notify_all();
  g_probeCondition.wait(lock, [] { return g_probeRelease; });
  return reinterpret_cast<HMODULE>(0x1234);
}

}  // namespace

TEST(DllLoadHookFilter, RefusesAFilteredLoadThroughEveryVariantAndLeavesOthersAlone) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();

  // Before a filter exists the load goes through.
  ASSERT_NE(LoadLibraryW(L"version.dll"), nullptr) << "precondition: version.dll loads";

  nevr_dll_load_hook::AddLoadFilter("test-filter", BlocksVersionDll);

  SetLastError(0);
  EXPECT_EQ(LoadLibraryW(L"version.dll"), nullptr);
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_MOD_NOT_FOUND));

  SetLastError(0);
  EXPECT_EQ(LoadLibraryExW(L"C:\\Windows\\System32\\VERSION.DLL", nullptr, 0), nullptr);
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_MOD_NOT_FOUND));

  SetLastError(0);
  EXPECT_EQ(LoadLibraryA("Version.dll"), nullptr);
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_MOD_NOT_FOUND));

  SetLastError(0);
  EXPECT_EQ(LoadLibraryExA("version.dll", nullptr, 0), nullptr);
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_MOD_NOT_FOUND));

  EXPECT_NE(LoadLibraryW(L"ws2_32.dll"), nullptr) << "a load no filter matches must go through";

  nevr_dll_load_hook::Shutdown();
  MH_Uninitialize();
}

TEST(DllLoadHookFilter, IsLoadBlockedNamesTheFilter) {
  nevr_dll_load_hook::AddLoadFilter("named-filter", BlocksVersionDll);
  const char* by = nullptr;
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"c:\\x\\version.dll", &by));
  ASSERT_NE(by, nullptr);
  EXPECT_STREQ(by, "named-filter");
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(L"kernel32.dll", &by));
  nevr_dll_load_hook::Shutdown();
}

TEST(DllLoadHookFilter, ReportedNameOutlivesFilterVectorChanges) {
  nevr_dll_load_hook::Shutdown();
  nevr_dll_load_hook::AddLoadFilter("original-filter", BlocksVersionDll);
  const char* by = nullptr;
  ASSERT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"version.dll", &by));
  ASSERT_NE(by, nullptr);
  nevr_dll_load_hook::Shutdown();
  nevr_dll_load_hook::AddLoadFilter("replacement-filter", BlocksVersionDll);
  EXPECT_STREQ(by, "original-filter");
  nevr_dll_load_hook::Shutdown();
}

TEST(DllLoadHookFilter, OverlongFilterNameIsTruncatedNotDropped) {
  nevr_dll_load_hook::Shutdown();
  // 60 bytes in; the stored name holds 47 plus the terminator.
  const std::string longName(60, 'n');
  nevr_dll_load_hook::AddLoadFilter(longName.c_str(), BlocksVersionDll);
  const char* by = nullptr;
  ASSERT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"version.dll", &by));
  ASSERT_NE(by, nullptr);
  EXPECT_EQ(std::string(by), std::string(47, 'n'));
  nevr_dll_load_hook::Shutdown();
}

TEST(DllLoadHookFilter, PredicateCanRegisterFilterForNextLoad) {
  nevr_dll_load_hook::Shutdown();
  g_addedFilter = false;
  g_predicateHeldLock = false;
  nevr_dll_load_hook::AddLoadFilter("registering-filter", AddsFilterDuringPredicate);
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(L"version.dll", nullptr));
  JoinLockProbes();
  EXPECT_FALSE(g_predicateHeldLock.load()) << "the registry mutex was held while a filter predicate ran";
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"version.dll", nullptr));
  nevr_dll_load_hook::Shutdown();
}

TEST(DllLoadHookCallbacks, ReentrantRegistrationFiresEachCallbackOnce) {
  nevr_dll_load_hook::Shutdown();
  g_firstCalls = 0;
  g_nestedCalls = 0;
  g_callbackHeldLock = false;
  nevr_dll_load_hook::OnLoad("nevr-reentrant-unloaded.dll", RegisterDuringCallback);
  HMODULE module = reinterpret_cast<HMODULE>(0x1234);
  nevr_dll_load_hook::FireCallbacksForModule("nevr-reentrant-unloaded.dll", module);
  nevr_dll_load_hook::FireCallbacksForModule("nevr-reentrant-unloaded.dll", module);
  JoinLockProbes();
  EXPECT_FALSE(g_callbackHeldLock.load()) << "the registry mutex was held while a patch callback ran";
  EXPECT_EQ(g_firstCalls.load(), 1);
  EXPECT_EQ(g_nestedCalls.load(), 1);
  nevr_dll_load_hook::Shutdown();
}

TEST(DllLoadHookCallbacks, OnLoadClaimsItsOwnRegistrationAfterConcurrentAppend) {
  nevr_dll_load_hook::Shutdown();
  g_immediateCalls = 0;
  g_laterCalls = 0;
  {
    std::lock_guard<std::mutex> lock(g_probeMutex);
    g_probeEntered = false;
    g_probeRelease = false;
  }
  nevr_dll_load_hook::SetModuleProbeForTest(BlockingModuleProbe);
  std::thread first([] {
    nevr_dll_load_hook::OnLoad("nevr-loaded-a.dll", ImmediateCallback);
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(g_probeMutex);
    entered = g_probeCondition.wait_for(lock, std::chrono::seconds(5), [] { return g_probeEntered; });
  }
  if (entered) nevr_dll_load_hook::OnLoad("nevr-pending-b.dll", LaterCallback);
  {
    std::lock_guard<std::mutex> lock(g_probeMutex);
    g_probeRelease = true;
  }
  g_probeCondition.notify_all();
  first.join();
  nevr_dll_load_hook::SetModuleProbeForTest(nullptr);
  ASSERT_TRUE(entered) << "module probe did not reach the forced interleaving";
  EXPECT_EQ(g_immediateCalls.load(), 1);
  nevr_dll_load_hook::FireCallbacksForModule("nevr-pending-b.dll", reinterpret_cast<HMODULE>(0x1234));
  EXPECT_EQ(g_laterCalls.load(), 1);
  nevr_dll_load_hook::Shutdown();
}

// #363 review: the Oculus predicate matches the file name, not any folder on the path.
TEST(OculusPlatformPath, RefusesTheSdkLibraryByFileName) {
  EXPECT_TRUE(nevr_dll_load_hook::IsOculusPlatformPath(L"libovrplatform64_1.dll"));
  EXPECT_TRUE(nevr_dll_load_hook::IsOculusPlatformPath(L"c:\\game\\bin\\libovrplatform64_1.dll"));
  EXPECT_TRUE(nevr_dll_load_hook::IsOculusPlatformPath(L"c:/game/bin/libovrplatform64_1.dll"));
}

TEST(OculusPlatformPath, AllowsAFileWhoseFolderIsNamedLikeTheSdk) {
  EXPECT_FALSE(nevr_dll_load_hook::IsOculusPlatformPath(L"c:\\mods\\ovrplatform\\helper.dll"));
  EXPECT_FALSE(nevr_dll_load_hook::IsOculusPlatformPath(L"c:/libovrplatform64_1/readme.dll"));
  EXPECT_FALSE(nevr_dll_load_hook::IsOculusPlatformPath(L"kernel32.dll"));
  EXPECT_FALSE(nevr_dll_load_hook::IsOculusPlatformPath(nullptr));
}
