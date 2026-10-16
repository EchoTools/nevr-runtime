// #361: the Oculus Platform SDK block installed its own LoadLibraryW/ExW detours on targets
// DllLoadHook had already hooked, so it failed with MH_ERROR_ALREADY_CREATED on every run. It is a
// load filter on DllLoadHook now; this runs the real hooks and checks the filter vetoes a load.

#include "runtime/hook/dll_load_hook.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/patch/pnsrad_load_queue.h"
#include "core/logging.h"

#include <MinHook.h>
#include <gtest/gtest.h>
#include <windows.h>
#include <winternl.h>

#include <cwchar>
#include <atomic>
#include <cstdint>
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

VOID Log(EchoVR::LogLevel, const CHAR*, ...) {}

TEST(DllLoadHookFilter, RefusesAFilteredLoadThroughEveryVariantAndLeavesOthersAlone) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  EXPECT_TRUE(nevr_hook_guard::IsOurDetour(reinterpret_cast<void*>(&LoadLibraryW)));
  EXPECT_TRUE(nevr_hook_guard::IsOurDetour(reinterpret_cast<void*>(&LoadLibraryExW)));

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

// Install() creates, enables and (on a failed enable) removes four MinHook hooks. The tests below
// replace those three calls so each failure branch runs and its cleanup is observed.
namespace {

struct FakeHookState {
  int createCalls = 0;
  int enableCalls = 0;
  int removeCalls = 0;
  std::vector<void*> removedTargets;
  MH_STATUS createResult = MH_OK;
  MH_STATUS enableResult = MH_OK;
  MH_STATUS removeResult = MH_OK;
  bool writeTrampoline = true;
};
FakeHookState g_fakeHooks;
int g_trampolineStorage = 0;

MH_STATUS WINAPI FakeCreate(LPVOID, LPVOID, LPVOID* original) {
  ++g_fakeHooks.createCalls;
  if (g_fakeHooks.createResult == MH_OK && g_fakeHooks.writeTrampoline && original != nullptr) {
    *original = &g_trampolineStorage;  // MinHook writes the trampoline only on MH_OK
  }
  return g_fakeHooks.createResult;
}
MH_STATUS WINAPI FakeEnable(LPVOID) {
  ++g_fakeHooks.enableCalls;
  return g_fakeHooks.enableResult;
}
MH_STATUS WINAPI FakeRemove(LPVOID target) {
  ++g_fakeHooks.removeCalls;
  g_fakeHooks.removedTargets.push_back(target);
  return g_fakeHooks.removeResult;
}

void UseFakeHooks() {
  g_fakeHooks = FakeHookState{};
  const nevr_dll_load_hook::MinHookOps ops{&FakeCreate, &FakeEnable, &FakeRemove};
  nevr_dll_load_hook::SetMinHookOpsForTest(&ops);
}

const char* const kVariants[] = {"LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW"};

}  // namespace

// MinHook writes the trampoline only when MH_CreateHook returns MH_OK. A second Install() fails every
// create with MH_ERROR_ALREADY_CREATED and leaves the first install's trampoline in place; the
// detours still call through it, so Install() must not clear it.
TEST(DllLoadHookInstall, AFailedCreateKeepsTheTrampolineOfTheHookThatIsInstalled) {
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  void* before[4];
  for (int i = 0; i < 4; ++i) {
    before[i] = nevr_dll_load_hook::OriginalForTest(kVariants[i]);
    ASSERT_NE(before[i], nullptr) << kVariants[i] << ": precondition: the first install worked";
  }

  UseFakeHooks();
  g_fakeHooks.createResult = MH_ERROR_ALREADY_CREATED;
  nevr_dll_load_hook::Install();

  EXPECT_EQ(g_fakeHooks.createCalls, 4);
  EXPECT_EQ(g_fakeHooks.removeCalls, 0) << "nothing was created, so there is nothing to remove";
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(nevr_dll_load_hook::OriginalForTest(kVariants[i]), before[i])
        << kVariants[i] << ": a failed create cleared the working trampoline";
  }
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
  EXPECT_NE(LoadLibraryW(L"ws2_32.dll"), nullptr) << "the installed hook must still load through its trampoline";
  nevr_dll_load_hook::Shutdown();
  MH_Uninitialize();
}

TEST(DllLoadHookInstall, AFailedEnableRemovesTheCreatedHookAndClearsItsTrampoline) {
  UseFakeHooks();
  g_fakeHooks.enableResult = MH_ERROR_MEMORY_PROTECT;
  const int recordedBefore = nevr_hook_guard::RecordedCount();
  nevr_dll_load_hook::Install();

  EXPECT_EQ(g_fakeHooks.createCalls, 4);
  EXPECT_EQ(g_fakeHooks.enableCalls, 4);
  EXPECT_EQ(g_fakeHooks.removeCalls, 4) << "every created-but-not-enabled hook must be removed";
  const void* expectedTargets[4] = {
      reinterpret_cast<void*>(&LoadLibraryA), reinterpret_cast<void*>(&LoadLibraryW),
      reinterpret_cast<void*>(&LoadLibraryExA), reinterpret_cast<void*>(&LoadLibraryExW)};
  ASSERT_EQ(g_fakeHooks.removedTargets.size(), 4U);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(g_fakeHooks.removedTargets[i], expectedTargets[i]) << kVariants[i];
    EXPECT_EQ(nevr_dll_load_hook::OriginalForTest(kVariants[i]), nullptr)
        << kVariants[i] << ": a removed hook's trampoline must not stay published";
  }
  EXPECT_EQ(nevr_hook_guard::RecordedCount(), recordedBefore) << "a hook that did not install must not be guarded";
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
  nevr_dll_load_hook::Shutdown();
}

TEST(DllLoadHookInstall, AFailedCleanupStillClearsTheTrampoline) {
  UseFakeHooks();
  g_fakeHooks.enableResult = MH_ERROR_MEMORY_PROTECT;
  g_fakeHooks.removeResult = MH_ERROR_NOT_CREATED;
  nevr_dll_load_hook::Install();

  EXPECT_EQ(g_fakeHooks.removeCalls, 4);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(nevr_dll_load_hook::OriginalForTest(kVariants[i]), nullptr) << kVariants[i];
  }
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
  nevr_dll_load_hook::Shutdown();
}

TEST(DllLoadHookInstall, ASuccessfulInstallRemovesNothing) {
  UseFakeHooks();
  nevr_dll_load_hook::Install();

  EXPECT_EQ(g_fakeHooks.removeCalls, 0);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(nevr_dll_load_hook::OriginalForTest(kVariants[i]), &g_trampolineStorage) << kVariants[i];
  }
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
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

namespace {

int g_fired = 0;
HMODULE g_module = nullptr;

void Count(const char*, HMODULE module) {
  g_fired++;
  g_module = module;
}

}  // namespace

// #593: a callback for a DLL that is already loaded when it registers fires right away, also during
// the runtime's boot (an injector such as Revive's loads dxgi/d3d12 before the runtime starts, #495).
// What made that unsafe was the callbacks' Log() reaching the game's CLog; Log() reports through the
// boot tee while the runtime boots (test_boot_log_routing).
TEST(DllLoadHookOnLoad, AnAlreadyLoadedDllsCallbackFiresAtOnceAfterInstall) {
  UseFakeHooks();
  nevr_dll_load_hook::Install();
  g_fired = 0;
  g_module = nullptr;
  nevr_dll_load_hook::OnLoad("KERNEL32.dll", Count);
  EXPECT_EQ(g_fired, 1) << "Install() must not delay the callback of a loaded DLL";
  EXPECT_EQ(g_module, GetModuleHandleA("kernel32.dll"));
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
  nevr_dll_load_hook::Shutdown();
}

// #504: the pnsrad patches are applied when the LoadLibrary call that mapped the module returns, not in the
// loader's DLL notification (which runs under the loader lock). This is the timing fixture: a real LoadLibraryW of a
// stand-in pnsrad.dll, the same record-then-apply path the runtime uses, and the caller's first read of the
// module right after LoadLibraryW returns. It fails if that read can see the unpatched byte.
namespace {

typedef struct {
  ULONG Flags;
  const UNICODE_STRING* FullDllName;
  const UNICODE_STRING* BaseDllName;
  PVOID DllBase;
  ULONG SizeOfImage;
} FixtureNotificationData;
typedef void(CALLBACK* FixtureNotificationFn)(ULONG, const FixtureNotificationData*, void*);
typedef NTSTATUS(NTAPI* RegisterNotificationFn)(ULONG, FixtureNotificationFn, void*, void**);
typedef NTSTATUS(NTAPI* UnregisterNotificationFn)(void*);

nevr_pnsrad_load::PendingLoads g_fixtureLoads;
std::atomic<bool> g_notifiedUnderLoaderLock{false};
std::atomic<int> g_observerRuns{0};
std::atomic<int> g_fixturePatches{0};

void CALLBACK RecordFixtureLoad(ULONG reason, const FixtureNotificationData* data, void*) {
  if (reason != nevr_matchmaker_host_patch::kNotificationLoaded || !data || !data->BaseDllName) return;
  if (nevr_dll_load_hook::LoaderLockHeldByThisThread()) g_notifiedUnderLoaderLock = true;
  const auto module = nevr_pnsrad_load::Classify(data->BaseDllName->Buffer, data->BaseDllName->Length / sizeof(WCHAR));
  if (module) g_fixtureLoads.Note(*module, reinterpret_cast<std::uintptr_t>(data->DllBase));
}

// Stands in for the pnsrad patches: rewrites the fixture's exported data word.
void PatchFixtureLoads() {
  ++g_observerRuns;
  g_fixtureLoads.Drain([](nevr_pnsrad_load::Module module, std::uintptr_t base) {
    if (module != nevr_pnsrad_load::Module::Pnsrad) return;
    auto* data = reinterpret_cast<int*>(GetProcAddress(reinterpret_cast<HMODULE>(base), "FixtureData"));
    if (data == nullptr) return;
    DWORD old = 0;
    if (!VirtualProtect(data, sizeof(int), PAGE_READWRITE, &old)) return;
    *data = 4321;
    VirtualProtect(data, sizeof(int), old, &old);
    ++g_fixturePatches;
  });
}

void CountObserverRun() { ++g_observerRuns; }

std::wstring FixturePnsradPath() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  dir.resize(dir.find_last_of(L"\\/") + 1);
  return dir + L"export-trace-fixture\\pnsrad.dll";
}

}  // namespace

TEST(DllLoadHookPostLoad, APatchAppliedAtLibraryReturnLandsBeforeTheCallersFirstRead) {
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  ASSERT_TRUE(nevr_hook_guard::IsOurDetour(reinterpret_cast<void*>(&LoadLibraryW)));
  nevr_dll_load_hook::AddPostLoadObserver(&PatchFixtureLoads);

  HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  auto reg = reinterpret_cast<RegisterNotificationFn>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
  auto unreg = reinterpret_cast<UnregisterNotificationFn>(GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
  ASSERT_NE(reg, nullptr);
  ASSERT_NE(unreg, nullptr);
  void* cookie = nullptr;
  ASSERT_EQ(reg(0, RecordFixtureLoad, nullptr, &cookie), 0);

  const std::wstring path = FixturePnsradPath();
  ASSERT_EQ(GetModuleHandleW(path.c_str()), nullptr) << "precondition: the fixture is not loaded yet";
  HMODULE module = LoadLibraryW(path.c_str());
  // The caller's first use of the module, as the game's CModule loader does right after LoadLibraryW.
  const auto* data = module ? reinterpret_cast<const int*>(GetProcAddress(module, "FixtureData")) : nullptr;
  const int firstRead = data ? *data : -1;

  unreg(cookie);
  ASSERT_NE(module, nullptr) << "fixture missing: export-trace-fixture\\pnsrad.dll next to the test";
  EXPECT_TRUE(g_notifiedUnderLoaderLock) << "the notification is expected to run under the loader lock";
  EXPECT_EQ(g_fixturePatches.load(), 1) << "the post-load observer did not patch the recorded load";
  EXPECT_EQ(firstRead, 4321) << "the caller read the module before its patch landed";
  FreeLibrary(module);
  nevr_dll_load_hook::Shutdown();
  MH_Uninitialize();
}

TEST(DllLoadHookPostLoad, ObserversWaitUntilTheCallingThreadReleasesTheLoaderLock) {
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  nevr_dll_load_hook::AddPostLoadObserver(&CountObserverRun);
  g_observerRuns = 0;

  ASSERT_FALSE(nevr_dll_load_hook::LoaderLockHeldByThisThread());
  ASSERT_NE(LoadLibraryW(L"ws2_32.dll"), nullptr);
  EXPECT_EQ(g_observerRuns.load(), 1) << "an observer runs after every LoadLibrary return";

  // A LoadLibrary call made while this thread holds the loader lock (as from inside a DllMain).
  auto* peb = reinterpret_cast<std::uint8_t*>(NtCurrentTeb()->ProcessEnvironmentBlock);
  auto* loaderLock = *reinterpret_cast<RTL_CRITICAL_SECTION**>(peb + 0x110);
  ASSERT_NE(loaderLock, nullptr);
  EnterCriticalSection(loaderLock);
  const bool heldInside = nevr_dll_load_hook::LoaderLockHeldByThisThread();
  HMODULE underLock = LoadLibraryW(L"ws2_32.dll");
  const int runsUnderLock = g_observerRuns.load();
  LeaveCriticalSection(loaderLock);

  EXPECT_TRUE(heldInside);
  EXPECT_NE(underLock, nullptr);
  EXPECT_EQ(runsUnderLock, 1) << "an observer ran while the calling thread held the loader lock";
  EXPECT_FALSE(nevr_dll_load_hook::LoaderLockHeldByThisThread());
  ASSERT_NE(LoadLibraryW(L"ws2_32.dll"), nullptr);
  EXPECT_EQ(g_observerRuns.load(), 2);
  nevr_dll_load_hook::Shutdown();
  MH_Uninitialize();
}
