// #361: the Oculus Platform SDK block installed its own LoadLibraryW/ExW detours on targets
// DllLoadHook had already hooked, so it failed with MH_ERROR_ALREADY_CREATED on every run. It is a
// load filter on DllLoadHook now; this runs the real hooks and checks the filter vetoes a load.

#include "runtime/hook/dll_load_hook.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/lifecycle/boot_hook_policy.h"
#include "runtime/patch/pnsrad_load_queue.h"
#include "core/logging.h"
#include "runtime/log/boot_log_tee.h"

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
#include <fstream>
#include <future>
#include <iterator>
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

// Each test leaves the process as it found it. TearDown resets what the tests in this file change, even
// after a fatal ASSERT: the fake MinHook ops, the module probe, DllLoadHook's registrations, filters
// and observers and its hooks (Shutdown), MinHook's initialisation (a fatal ASSERT that skipped
// MH_Uninitialize made the next MH_Initialize return MH_ERROR_ALREADY_INITIALIZED), the four trampoline
// pointers (Shutdown only disables the hooks, so they would name freed trampolines), and the hook
// guard's records. The guard keeps the first record it finds for an address, so a record left by one
// test (a fake install records LoadLibraryW's unhooked bytes) made IsOurDetour false for the next test
// that hooks LoadLibraryW for real (#633). The first test starts from the process's clean state, and SetUp
// fails a test that starts with a trampoline or a hook-guard record left behind.
class DllLoadHookIsolation : public testing::Test {
 protected:
  // Checks, does not reset: a test that leaves a trampoline or a hook-guard record behind fails the next one here.
  void SetUp() override {
    for (const char* variant : {"LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW"}) {
      ASSERT_EQ(nevr_dll_load_hook::OriginalForTest(variant), nullptr) << variant << ": a trampoline outlived the previous test";
    }
    ASSERT_EQ(nevr_hook_guard::RecordedCount(), 0) << "a hook-guard record outlived the previous test";
  }

  void TearDown() override {
    nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
    nevr_dll_load_hook::SetModuleProbeForTest(nullptr);
    nevr_dll_load_hook::Shutdown();  // disables the hooks, then clears registrations, filters, observers
    MH_Uninitialize();               // MH_ERROR_NOT_INITIALIZED, ignored, when the test never initialised it
    nevr_dll_load_hook::ResetTrampolinesForTest();
    nevr_hook_guard::ResetForTest();
  }
};
class DllLoadHookFilter : public DllLoadHookIsolation {};
class DllLoadHookRewriter : public DllLoadHookIsolation {};
class DllLoadHookCallbacks : public DllLoadHookIsolation {};
class DllLoadHookInstall : public DllLoadHookIsolation {};
class DllLoadHookOnLoad : public DllLoadHookIsolation {};
class DllLoadHookPostLoad : public DllLoadHookIsolation {};

TEST_F(DllLoadHookFilter, RefusesAFilteredLoadThroughEveryVariantAndLeavesOthersAlone) {
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
}

TEST_F(DllLoadHookFilter, IsLoadBlockedNamesTheFilter) {
  nevr_dll_load_hook::AddLoadFilter("named-filter", BlocksVersionDll);
  const char* by = nullptr;
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"c:\\x\\version.dll", &by));
  ASSERT_NE(by, nullptr);
  EXPECT_STREQ(by, "named-filter");
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(L"kernel32.dll", &by));
}

TEST_F(DllLoadHookFilter, ReportedNameOutlivesFilterVectorChanges) {
  nevr_dll_load_hook::AddLoadFilter("original-filter", BlocksVersionDll);
  const char* by = nullptr;
  ASSERT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"version.dll", &by));
  ASSERT_NE(by, nullptr);
  nevr_dll_load_hook::Shutdown();
  nevr_dll_load_hook::AddLoadFilter("replacement-filter", BlocksVersionDll);
  EXPECT_STREQ(by, "original-filter");
}

TEST_F(DllLoadHookFilter, OverlongFilterNameIsTruncatedNotDropped) {
  // 60 bytes in; the stored name holds 47 plus the terminator.
  const std::string longName(60, 'n');
  nevr_dll_load_hook::AddLoadFilter(longName.c_str(), BlocksVersionDll);
  const char* by = nullptr;
  ASSERT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"version.dll", &by));
  ASSERT_NE(by, nullptr);
  EXPECT_EQ(std::string(by), std::string(47, 'n'));
}

TEST_F(DllLoadHookFilter, PredicateCanRegisterFilterForNextLoad) {
  g_addedFilter = false;
  g_predicateHeldLock = false;
  nevr_dll_load_hook::AddLoadFilter("registering-filter", AddsFilterDuringPredicate);
  EXPECT_FALSE(nevr_dll_load_hook::IsLoadBlocked(L"version.dll", nullptr));
  JoinLockProbes();
  EXPECT_FALSE(g_predicateHeldLock.load()) << "the registry mutex was held while a filter predicate ran";
  EXPECT_TRUE(nevr_dll_load_hook::IsLoadBlocked(L"version.dll", nullptr));
}

TEST_F(DllLoadHookCallbacks, ReentrantRegistrationFiresEachCallbackOnce) {
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
}

TEST_F(DllLoadHookCallbacks, OnLoadClaimsItsOwnRegistrationAfterConcurrentAppend) {
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
  ASSERT_TRUE(entered) << "module probe did not reach the forced interleaving";
  EXPECT_EQ(g_immediateCalls.load(), 1);
  nevr_dll_load_hook::FireCallbacksForModule("nevr-pending-b.dll", reinterpret_cast<HMODULE>(0x1234));
  EXPECT_EQ(g_laterCalls.load(), 1);
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
  // When set, createResult and enableResult apply to this target only; every other target succeeds.
  void* onlyTarget = nullptr;
};
FakeHookState g_fakeHooks;
int g_trampolineStorage = 0;

MH_STATUS WINAPI FakeCreate(LPVOID target, LPVOID, LPVOID* original) {
  ++g_fakeHooks.createCalls;
  const MH_STATUS result =
      g_fakeHooks.onlyTarget == nullptr || g_fakeHooks.onlyTarget == target ? g_fakeHooks.createResult : MH_OK;
  if (result == MH_OK && g_fakeHooks.writeTrampoline && original != nullptr) {
    *original = &g_trampolineStorage;  // MinHook writes the trampoline only on MH_OK
  }
  return result;
}
MH_STATUS WINAPI FakeEnable(LPVOID target) {
  ++g_fakeHooks.enableCalls;
  return g_fakeHooks.onlyTarget == nullptr || g_fakeHooks.onlyTarget == target ? g_fakeHooks.enableResult : MH_OK;
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

// What the boot tee wrote to nevr-boot.jsonl while `install` ran (#632). Install() runs inside the runtime's
// boot phase, so its report must reach this file, not only stderr. The window is exact: from the file size
// at Init() to the file size when `install` returns, so an absence assertion over it cannot be satisfied or
// broken by what earlier runs appended to the shared, append-only file. The tee is closed on every path.
struct BootTeeScope {
  BootTeeScope() { nevr_boot_log_tee::Init(); }
  ~BootTeeScope() { nevr_boot_log_tee::Close(); }
  BootTeeScope(const BootTeeScope&) = delete;
  BootTeeScope& operator=(const BootTeeScope&) = delete;
};

std::streamoff FileSizeOf(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  return in ? static_cast<std::streamoff>(in.tellg()) : 0;
}

template <typename Fn>
std::string BootFileTextWrittenBy(Fn install) {
  BootTeeScope tee;
  const std::string path = nevr_boot_log_tee::Path();
  if (path.empty()) return "<the boot tee could not resolve nevr-boot.jsonl>";
  const std::streamoff start = FileSizeOf(path);
  install();
  const std::streamoff end = FileSizeOf(path);
  std::ifstream in(path, std::ios::binary);
  in.seekg(start);
  std::string text(static_cast<std::size_t>(end - start), '\0');
  in.read(&text[0], end - start);
  return text;
}

}  // namespace

// MinHook writes the trampoline only when MH_CreateHook returns MH_OK. A second Install() fails every
// create with MH_ERROR_ALREADY_CREATED and leaves the first install's trampoline in place; the
// detours still call through it, so Install() must not clear it.
TEST_F(DllLoadHookInstall, AFailedCreateKeepsTheTrampolineOfTheHookThatIsInstalled) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  void* before[4];
  for (int i = 0; i < 4; ++i) {
    before[i] = nevr_dll_load_hook::OriginalForTest(kVariants[i]);
    ASSERT_NE(before[i], nullptr) << kVariants[i] << ": precondition: the first install worked";
  }

  UseFakeHooks();
  g_fakeHooks.createResult = MH_ERROR_ALREADY_CREATED;
  testing::internal::CaptureStderr();
  nevr_dll_load_hook::Install();
  const std::string err = testing::internal::GetCapturedStderr();
  // #630: the hook from the first install is live, so the line must not say its loads lose the filters.
  EXPECT_NE(err.find("hook FAILED name=LoadLibraryA reason=MH_ERROR_ALREADY_CREATED — a hook created earlier stays in "
                     "place"),
            std::string::npos)
      << err;
  EXPECT_EQ(err.find("run no load filter"), std::string::npos) << err;

  EXPECT_EQ(g_fakeHooks.createCalls, 4);
  EXPECT_EQ(g_fakeHooks.removeCalls, 0) << "nothing was created, so there is nothing to remove";
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(nevr_dll_load_hook::OriginalForTest(kVariants[i]), before[i])
        << kVariants[i] << ": a failed create cleared the working trampoline";
  }
  nevr_dll_load_hook::SetMinHookOpsForTest(nullptr);
  EXPECT_NE(LoadLibraryW(L"ws2_32.dll"), nullptr) << "the installed hook must still load through its trampoline";
}

TEST_F(DllLoadHookInstall, AFailedEnableRemovesTheCreatedHookAndClearsItsTrampoline) {
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
}

// #630: the failure line says what an unhooked variant loses (its loads run no filter, observer or callback)
// and claims no search-path hardening.
TEST_F(DllLoadHookInstall, AFailedHookSaysItsLoadsRunNoFilterObserverOrCallback) {
  UseFakeHooks();
  g_fakeHooks.enableResult = MH_ERROR_MEMORY_PROTECT;
  testing::internal::CaptureStderr();
  nevr_dll_load_hook::Install();
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_NE(err.find("hook FAILED name=LoadLibraryA reason=MH_ERROR_MEMORY_PROTECT — loads through this variant run "
                     "no load filter, observer or callback"),
            std::string::npos)
      << err;
  EXPECT_EQ(err.find("hardening"), std::string::npos) << err;
}

// #630: a create that fails for any other reason (a first Install() that could not hook) leaves no hook, so its
// line says the variant's loads run no filter, observer or callback, never that a hook stays in place.
TEST_F(DllLoadHookInstall, ACreateThatFailsForAnotherReasonSaysItsLoadsRunNoFilter) {
  UseFakeHooks();
  g_fakeHooks.createResult = MH_ERROR_NOT_EXECUTABLE;
  testing::internal::CaptureStderr();
  nevr_dll_load_hook::Install();
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_NE(err.find("hook FAILED name=LoadLibraryA reason=MH_ERROR_NOT_EXECUTABLE — loads through this variant run no "
                     "load filter, observer or callback"),
            std::string::npos)
      << err;
  EXPECT_EQ(err.find("stays in place"), std::string::npos) << err;
  EXPECT_EQ(g_fakeHooks.removeCalls, 0) << "nothing was created, so there is nothing to remove";
}

TEST_F(DllLoadHookInstall, AFailedCleanupStillClearsTheTrampoline) {
  UseFakeHooks();
  g_fakeHooks.enableResult = MH_ERROR_MEMORY_PROTECT;
  g_fakeHooks.removeResult = MH_ERROR_NOT_CREATED;
  nevr_dll_load_hook::Install();

  EXPECT_EQ(g_fakeHooks.removeCalls, 4);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(nevr_dll_load_hook::OriginalForTest(kVariants[i]), nullptr) << kVariants[i];
  }
}

TEST_F(DllLoadHookInstall, ASuccessfulInstallRemovesNothing) {
  UseFakeHooks();
  nevr_dll_load_hook::Install();

  EXPECT_EQ(g_fakeHooks.removeCalls, 0);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(nevr_dll_load_hook::OriginalForTest(kVariants[i]), &g_trampolineStorage) << kVariants[i];
  }
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
TEST_F(DllLoadHookOnLoad, AnAlreadyLoadedDllsCallbackFiresAtOnceAfterInstall) {
  UseFakeHooks();
  nevr_dll_load_hook::Install();
  g_fired = 0;
  g_module = nullptr;
  nevr_dll_load_hook::OnLoad("KERNEL32.dll", Count);
  EXPECT_EQ(g_fired, 1) << "Install() must not delay the callback of a loaded DLL";
  EXPECT_EQ(g_module, GetModuleHandleA("kernel32.dll"));
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

TEST_F(DllLoadHookPostLoad, APatchAppliedAtLibraryReturnLandsBeforeTheCallersFirstRead) {
  // Everything this test reads is reset here, so an earlier run of it (--gtest_repeat) leaves nothing behind.
  g_notifiedUnderLoaderLock = false;
  g_fixturePatches = 0;
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
  // Unregisters on every exit, so a failed ASSERT below cannot leave RecordFixtureLoad registered.
  struct NotificationGuard {
    UnregisterNotificationFn unregister;
    void* cookie;
    void Release() {
      if (cookie != nullptr) unregister(cookie);
      cookie = nullptr;
    }
    ~NotificationGuard() { Release(); }
  } notification{unreg, cookie};

  const std::wstring path = FixturePnsradPath();
  ASSERT_EQ(GetModuleHandleW(path.c_str()), nullptr) << "precondition: the fixture is not loaded yet";
  HMODULE module = LoadLibraryW(path.c_str());
  // The caller's first use of the module, as the game's CModule loader does right after LoadLibraryW.
  const auto* data = module ? reinterpret_cast<const int*>(GetProcAddress(module, "FixtureData")) : nullptr;
  const int firstRead = data ? *data : -1;

  notification.Release();
  ASSERT_NE(module, nullptr) << "fixture missing: export-trace-fixture\\pnsrad.dll next to the test";
  EXPECT_TRUE(g_notifiedUnderLoaderLock) << "the notification is expected to run under the loader lock";
  EXPECT_EQ(g_fixturePatches.load(), 1) << "the post-load observer did not patch the recorded load";
  EXPECT_EQ(firstRead, 4321) << "the caller read the module before its patch landed";
  FreeLibrary(module);
}

TEST_F(DllLoadHookPostLoad, ObserversWaitUntilTheCallingThreadReleasesTheLoaderLock) {
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
}

// #632: the per-variant failure line, with its MH_STATUS, and the PARTIAL summary reach the boot file.
TEST_F(DllLoadHookInstall, AFailedVariantAndThePartialSummaryReachTheBootFile) {
  UseFakeHooks();
  g_fakeHooks.onlyTarget = reinterpret_cast<void*>(&LoadLibraryExW);
  g_fakeHooks.enableResult = MH_ERROR_MEMORY_PROTECT;
  testing::internal::CaptureStderr();
  const std::string boot = BootFileTextWrittenBy([] { nevr_dll_load_hook::Install(); });
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] warn; hook FAILED name=LoadLibraryExW reason=MH_ERROR_MEMORY_PROTECT"), std::string::npos)
      << "boot file was:\n" << boot;
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] warn; LoadLibrary hooks PARTIAL (3/4)"), std::string::npos) << "boot file was:\n" << boot;
  EXPECT_EQ(boot.find("LoadLibrary hooks OK"), std::string::npos) << boot;
  EXPECT_NE(err.find("LoadLibrary hooks PARTIAL (3/4)"), std::string::npos) << "the tee still writes stderr: " << err;
}

// #632: the failed-hook cleanup line is a report line too.
TEST_F(DllLoadHookInstall, AFailedCleanupReachesTheBootFile) {
  UseFakeHooks();
  g_fakeHooks.onlyTarget = reinterpret_cast<void*>(&LoadLibraryW);
  g_fakeHooks.enableResult = MH_ERROR_MEMORY_PROTECT;
  g_fakeHooks.removeResult = MH_ERROR_NOT_CREATED;
  const std::string boot = BootFileTextWrittenBy([] { nevr_dll_load_hook::Install(); });
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] warn; failed-hook cleanup FAILED name=LoadLibraryW reason=MH_ERROR_NOT_CREATED"),
            std::string::npos)
      << "boot file was:\n" << boot;
}

// #632: a full install says OK (4/4) in the boot file and no FAILED line.
TEST_F(DllLoadHookInstall, AFullInstallSaysOkInTheBootFile) {
  UseFakeHooks();
  const std::string boot = BootFileTextWrittenBy([] { nevr_dll_load_hook::Install(); });
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] info; LoadLibrary hooks OK (4/4)"), std::string::npos) << "boot file was:\n" << boot;
  EXPECT_EQ(boot.find("FAILED"), std::string::npos) << boot;
}

// #632: Install() returns how many of the four variants are hooked, so the caller can say what happened.
TEST_F(DllLoadHookInstall, InstallReturnsTheNumberOfVariantsHooked) {
  UseFakeHooks();
  EXPECT_EQ(nevr_dll_load_hook::Install(), nevr_dll_load_hook::kLoadLibraryVariantCount);
  EXPECT_EQ(nevr_dll_load_hook::kLoadLibraryVariantCount, 4);
}

TEST_F(DllLoadHookInstall, InstallReturnsThreeWhenOneVariantFailsToEnable) {
  UseFakeHooks();
  g_fakeHooks.onlyTarget = reinterpret_cast<void*>(&LoadLibraryExA);
  g_fakeHooks.enableResult = MH_ERROR_MEMORY_PROTECT;
  EXPECT_EQ(nevr_dll_load_hook::Install(), 3);
}

TEST_F(DllLoadHookInstall, InstallReturnsZeroWhenEveryCreateFails) {
  UseFakeHooks();
  g_fakeHooks.createResult = MH_ERROR_NOT_EXECUTABLE;
  EXPECT_EQ(nevr_dll_load_hook::Install(), 0);
}

// #632: the four LoadLibrary hooks are REQUIRED. A partial install fails the boot (a server then
// refuses to start, a client's last boot line says ok=false) and records the count as its cause. The verdict is
// a pure function so it runs here; InitializeAfterGameImageGuard cannot run in a GTest, so
// tools/tests/test_runtime_lifecycle_invariants.py pins that initialize.cpp acts on it.
TEST(DllLoadHookBootVerdict, TheLoadLibraryHooksAreRequired) {
  EXPECT_TRUE(nevr_boot_hook_policy::kLoadLibraryHooksRequired);
}

TEST(DllLoadHookBootVerdict, AFullInstallFailsNothingAndRecordsNoCause) {
  const auto verdict = nevr_boot_hook_policy::JudgeLoadLibraryHooks(4, 4);
  EXPECT_TRUE(verdict.complete);
  EXPECT_FALSE(verdict.failsBoot);
  EXPECT_STREQ(verdict.cause, "");
}

TEST(DllLoadHookBootVerdict, APartialInstallFailsTheBootAndNamesTheCount) {
  for (int installed = 0; installed < 4; ++installed) {
    const auto verdict = nevr_boot_hook_policy::JudgeLoadLibraryHooks(installed, 4);
    EXPECT_FALSE(verdict.complete) << installed;
    EXPECT_TRUE(verdict.failsBoot) << installed;
    const std::string expected = "LoadLibrary hooks " + std::to_string(installed) + "/4 installed";
    EXPECT_EQ(std::string(verdict.cause), expected);
  }
}

// #654: the hook's own lines (a refused load, a fired patch callback, a truncated filter name, a DLL that was
// already loaded) reach the durable log, not only stderr. Boot phase: through the boot tee, into
// nevr-boot.jsonl. After boot: through Log() to the game's CLog, stood in for by a recorder. Under the loader
// lock after boot, CLog is not usable (#495): the line waits and is written by the next LoadLibrary that
// returns outside the lock.
namespace {

struct ClogLine {
  EchoVR::LogLevel level;
  std::string text;
};
std::vector<ClogLine> g_clogLines;

VOID RecordingClogForHook(EchoVR::LogLevel level, UINT64, const CHAR* format, va_list args) {
  char buf[512];
  vsnprintf(buf, sizeof(buf), format, args);
  g_clogLines.push_back({level, buf});
}

struct RecordingClogScope {
  EchoVR::WriteLogFunc* saved;
  RecordingClogScope() : saved(EchoVR::WriteLog) {
    EchoVR::WriteLog = &RecordingClogForHook;
    g_clogLines.clear();
  }
  ~RecordingClogScope() { EchoVR::WriteLog = saved; }
  RecordingClogScope(const RecordingClogScope&) = delete;
  RecordingClogScope& operator=(const RecordingClogScope&) = delete;
};

bool ClogHas(EchoVR::LogLevel level, const std::string& needle) {
  for (const ClogLine& line : g_clogLines) {
    if (line.level == level && line.text.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::string AllClogText() {
  std::string all;
  for (const ClogLine& line : g_clogLines) all += line.text + "\n";
  return all;
}

}  // namespace

TEST_F(DllLoadHookFilter, ARefusedLoadIsLoggedDurablyInBootAndAfter) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  const std::string boot = BootFileTextWrittenBy([] {
    nevr_dll_load_hook::Install();
    nevr_dll_load_hook::AddLoadFilter("test-filter", BlocksVersionDll);
    EXPECT_EQ(LoadLibraryW(L"version.dll"), nullptr);
  });
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] info; load refused by filter 'test-filter': version.dll"), std::string::npos)
      << "boot file was:\n" << boot;

  RecordingClogScope clog;
  EXPECT_EQ(LoadLibraryW(L"version.dll"), nullptr);
  EXPECT_TRUE(ClogHas(EchoVR::LogLevel::Info, "[NEVR.DLLHOOK] load refused by filter 'test-filter': version.dll"))
      << AllClogText();
}

TEST_F(DllLoadHookFilter, ARefusalMadeUnderTheLoaderLockIsWrittenAfterItIsReleased) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  nevr_dll_load_hook::AddLoadFilter("test-filter", BlocksVersionDll);
  RecordingClogScope clog;

  auto* peb = reinterpret_cast<std::uint8_t*>(NtCurrentTeb()->ProcessEnvironmentBlock);
  auto* loaderLock = *reinterpret_cast<RTL_CRITICAL_SECTION**>(peb + 0x110);
  ASSERT_NE(loaderLock, nullptr);
  EnterCriticalSection(loaderLock);
  EXPECT_EQ(LoadLibraryW(L"version.dll"), nullptr);
  const std::size_t writtenUnderLock = g_clogLines.size();
  LeaveCriticalSection(loaderLock);

  EXPECT_EQ(writtenUnderLock, 0u) << "CLog was called while this thread held the loader lock";
  EXPECT_NE(LoadLibraryW(L"ws2_32.dll"), nullptr);
  EXPECT_TRUE(ClogHas(EchoVR::LogLevel::Info, "[NEVR.DLLHOOK] load refused by filter 'test-filter': version.dll"))
      << "the held line must be written by the next load outside the lock: " << AllClogText();
}

TEST_F(DllLoadHookCallbacks, AFiredPatchCallbackIsLoggedDurablyInBootAndAfter) {
  g_fired = 0;
  const HMODULE module = reinterpret_cast<HMODULE>(0x1234);
  nevr_dll_load_hook::OnLoad("nevr-fire-a.dll", Count);
  nevr_dll_load_hook::OnLoad("nevr-fire-b.dll", Count);
  const std::string boot = BootFileTextWrittenBy([&] { nevr_dll_load_hook::FireCallbacksForModule("nevr-fire-a.dll", module); });
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] info; firing patch callback for 'nevr-fire-a.dll' (module="), std::string::npos)
      << "boot file was:\n" << boot;

  RecordingClogScope clog;
  nevr_dll_load_hook::FireCallbacksForModule("nevr-fire-b.dll", module);
  EXPECT_TRUE(ClogHas(EchoVR::LogLevel::Info, "[NEVR.DLLHOOK] firing patch callback for 'nevr-fire-b.dll' (module="))
      << AllClogText();
}

TEST_F(DllLoadHookFilter, ATruncatedFilterNameIsLoggedAsAWarningInBootAndAfter) {
  const std::string longName(60, 'n');
  const std::string boot = BootFileTextWrittenBy([&] { nevr_dll_load_hook::AddLoadFilter(longName.c_str(), BlocksVersionDll); });
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] warn; load filter name truncated to 47 bytes"), std::string::npos)
      << "boot file was:\n" << boot;

  RecordingClogScope clog;
  nevr_dll_load_hook::AddLoadFilter(longName.c_str(), BlocksVersionDll);
  EXPECT_TRUE(ClogHas(EchoVR::LogLevel::Warning, "[NEVR.DLLHOOK] load filter name truncated to 47 bytes"))
      << AllClogText();
}

TEST_F(DllLoadHookOnLoad, AnAlreadyLoadedDllIsLoggedDurablyInBootAndAfter) {
  UseFakeHooks();
  nevr_dll_load_hook::Install();
  g_fired = 0;
  const std::string boot = BootFileTextWrittenBy([] { nevr_dll_load_hook::OnLoad("KERNEL32.dll", Count); });
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] info; 'kernel32.dll' already loaded (module="), std::string::npos)
      << "boot file was:\n" << boot;

  RecordingClogScope clog;
  nevr_dll_load_hook::OnLoad("USER32.dll", Count);
  EXPECT_TRUE(ClogHas(EchoVR::LogLevel::Info, "[NEVR.DLLHOOK] 'user32.dll' already loaded (module="))
      << AllClogText();
}

// A refusal made while this thread holds the loader lock, in the boot phase: Log() goes to the tee, so the line is
// written at once, not held. (The test takes the lock itself; initialization no longer runs under it.)
TEST_F(DllLoadHookFilter, ARefusalMadeUnderTheLoaderLockInTheBootPhaseIsWrittenAtOnce) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  nevr_dll_load_hook::AddLoadFilter("test-filter", BlocksVersionDll);
  auto* peb = reinterpret_cast<std::uint8_t*>(NtCurrentTeb()->ProcessEnvironmentBlock);
  auto* loaderLock = *reinterpret_cast<RTL_CRITICAL_SECTION**>(peb + 0x110);
  ASSERT_NE(loaderLock, nullptr);
  const std::string boot = BootFileTextWrittenBy([&] {
    EnterCriticalSection(loaderLock);
    EXPECT_EQ(LoadLibraryW(L"version.dll"), nullptr);
    LeaveCriticalSection(loaderLock);
  });
  EXPECT_NE(boot.find("[NEVR.DLLHOOK] info; load refused by filter 'test-filter': version.dll"), std::string::npos)
      << "boot file was:\n" << boot;
}

// #688: the buffer for lines made under the loader lock holds eight. A ninth is counted, and the drain says
// how many were dropped, once.
namespace {

RTL_CRITICAL_SECTION* LoaderLockForTest() {
  auto* peb = reinterpret_cast<std::uint8_t*>(NtCurrentTeb()->ProcessEnvironmentBlock);
  return *reinterpret_cast<RTL_CRITICAL_SECTION**>(peb + 0x110);
}

std::size_t ClogCount(const std::string& needle) {
  std::size_t n = 0;
  for (const ClogLine& line : g_clogLines) {
    if (line.text.find(needle) != std::string::npos) ++n;
  }
  return n;
}

}  // namespace

TEST_F(DllLoadHookFilter, NineLinesUnderTheLoaderLockKeepEightAndCountTheDroppedOne) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  nevr_dll_load_hook::AddLoadFilter("test-filter", BlocksVersionDll);
  RecordingClogScope clog;
  RTL_CRITICAL_SECTION* const lock = LoaderLockForTest();
  ASSERT_NE(lock, nullptr);
  EnterCriticalSection(lock);
  for (int i = 0; i < 9; ++i) EXPECT_EQ(LoadLibraryW(L"version.dll"), nullptr);
  LeaveCriticalSection(lock);
  ASSERT_NE(LoadLibraryW(L"ws2_32.dll"), nullptr);

  EXPECT_EQ(ClogCount("load refused by filter 'test-filter'"), 8u) << AllClogText();
  EXPECT_TRUE(ClogHas(EchoVR::LogLevel::Warning, "[NEVR.DLLHOOK] 1 line(s) made under the loader lock were dropped"))
      << AllClogText();

  // The count is reported once: the next drain finds nothing to say.
  g_clogLines.clear();
  ASSERT_NE(LoadLibraryW(L"ws2_32.dll"), nullptr);
  EXPECT_EQ(ClogCount("dropped"), 0u) << AllClogText();
}

// #688: graceful shutdown writes what a load under the lock left waiting, and does not touch CLog while
// the calling thread still holds the lock.
TEST_F(DllLoadHookFilter, FlushPendingLinesWritesAHeldLineOutsideTheLockOnly) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  nevr_dll_load_hook::Install();
  nevr_dll_load_hook::AddLoadFilter("test-filter", BlocksVersionDll);
  RecordingClogScope clog;
  RTL_CRITICAL_SECTION* const lock = LoaderLockForTest();
  ASSERT_NE(lock, nullptr);
  EnterCriticalSection(lock);
  EXPECT_EQ(LoadLibraryW(L"version.dll"), nullptr);
  nevr_dll_load_hook::FlushPendingLines();
  const std::size_t writtenUnderLock = g_clogLines.size();
  LeaveCriticalSection(lock);
  EXPECT_EQ(writtenUnderLock, 0u) << "FlushPendingLines wrote while this thread held the loader lock";

  nevr_dll_load_hook::FlushPendingLines();
  EXPECT_TRUE(ClogHas(EchoVR::LogLevel::Info, "[NEVR.DLLHOOK] load refused by filter 'test-filter': version.dll"))
      << AllClogText();
}

// #1177: a rewriter names the file a load no filter refused maps. The hook calls the real wide loader with that path and
// the caller's flags, whichever of the four variants the caller used.
namespace {
bool RewriteToWs2(const wchar_t* lower, std::wstring* path) {
  if (std::wcscmp(lower, L"nevr-rewrite-me.dll") != 0) return false;
  *path = L"ws2_32.dll";
  return true;
}
bool RewriteToMsimg32(const wchar_t* lower, std::wstring* path) {
  if (std::wcscmp(lower, L"nevr-rewrite-me.dll") != 0) return false;
  *path = L"msimg32.dll";
  return true;
}
bool RefuseRewriteMe(const wchar_t* lower) { return std::wcscmp(lower, L"nevr-rewrite-me.dll") == 0; }
}  // namespace

TEST_F(DllLoadHookRewriter, EveryVariantLoadsThePathTheRewriterNames) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  ASSERT_EQ(nevr_dll_load_hook::Install(), 4);
  nevr_dll_load_hook::AddLoadRewriter("test-rewriter", &RewriteToWs2);
  const HMODULE expected = LoadLibraryW(L"ws2_32.dll");
  ASSERT_NE(expected, nullptr);
  EXPECT_EQ(LoadLibraryW(L"nevr-rewrite-me.dll"), expected) << "LoadLibraryW";
  EXPECT_EQ(LoadLibraryExW(L"nevr-rewrite-me.dll", nullptr, 0), expected) << "LoadLibraryExW";
  EXPECT_EQ(LoadLibraryA("nevr-rewrite-me.dll"), expected) << "LoadLibraryA";
  EXPECT_EQ(LoadLibraryExA("nevr-rewrite-me.dll", nullptr, 0), expected) << "LoadLibraryExA";
}

TEST_F(DllLoadHookRewriter, TheCallersFlagsGoToTheRewrittenLoad) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  ASSERT_EQ(nevr_dll_load_hook::Install(), 4);
  nevr_dll_load_hook::AddLoadRewriter("test-rewriter", &RewriteToMsimg32);
  ASSERT_EQ(GetModuleHandleW(L"msimg32.dll"), nullptr) << "precondition: the target is not already loaded";
  // A module mapped as a data file has the low bits of its handle set; one mapped by a load with no flags does not.
  const HMODULE asData = LoadLibraryExW(L"nevr-rewrite-me.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
  ASSERT_NE(asData, nullptr) << GetLastError();
  EXPECT_NE(reinterpret_cast<std::uintptr_t>(asData) & 3u, 0u) << "the flags did not reach the loader";
  FreeLibrary(asData);
  const HMODULE asDataA = LoadLibraryExA("nevr-rewrite-me.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
  ASSERT_NE(asDataA, nullptr) << GetLastError();
  EXPECT_NE(reinterpret_cast<std::uintptr_t>(asDataA) & 3u, 0u) << "the ANSI variant dropped the flags";
  FreeLibrary(asDataA);
}

TEST_F(DllLoadHookRewriter, ARefusedLoadIsNotRewritten) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  ASSERT_EQ(nevr_dll_load_hook::Install(), 4);
  nevr_dll_load_hook::AddLoadFilter("test-filter", &RefuseRewriteMe);
  nevr_dll_load_hook::AddLoadRewriter("test-rewriter", &RewriteToWs2);
  SetLastError(0);
  EXPECT_EQ(LoadLibraryW(L"nevr-rewrite-me.dll"), nullptr);
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_MOD_NOT_FOUND));
}

TEST_F(DllLoadHookRewriter, AShutdownDropsTheRewritersAndANullOneIsIgnored) {
  nevr_dll_load_hook::AddLoadRewriter("null-rewriter", nullptr);
  std::wstring path;
  EXPECT_FALSE(nevr_dll_load_hook::RewriteLoad(L"nevr-rewrite-me.dll", &path));
  nevr_dll_load_hook::AddLoadRewriter("test-rewriter", &RewriteToWs2);
  EXPECT_TRUE(nevr_dll_load_hook::RewriteLoad(L"nevr-rewrite-me.dll", &path));
  EXPECT_EQ(path, L"ws2_32.dll");
  nevr_dll_load_hook::Shutdown();
  path.clear();
  EXPECT_FALSE(nevr_dll_load_hook::RewriteLoad(L"nevr-rewrite-me.dll", &path));
}

// The wide Ex hook failed to install, so there is no trampoline for it: a load the narrow hooks rewrite goes to the
// real LoadLibraryExW, which no hook is on.
TEST_F(DllLoadHookRewriter, ARewrittenLoadStillLoadsWhenTheWideExHookIsNotInstalled) {
  ASSERT_EQ(MH_Initialize(), MH_OK);
  const nevr_dll_load_hook::MinHookOps ops{
      [](LPVOID target, LPVOID detour, LPVOID* original) -> MH_STATUS {
        if (target == reinterpret_cast<LPVOID>(&LoadLibraryExW)) return MH_ERROR_UNSUPPORTED_FUNCTION;
        return MH_CreateHook(target, detour, original);
      },
      nullptr, nullptr};
  nevr_dll_load_hook::SetMinHookOpsForTest(&ops);
  ASSERT_EQ(nevr_dll_load_hook::Install(), 3);
  ASSERT_EQ(nevr_dll_load_hook::OriginalForTest("LoadLibraryExW"), nullptr);
  nevr_dll_load_hook::AddLoadRewriter("test-rewriter", &RewriteToWs2);
  const HMODULE expected = LoadLibraryExW(L"ws2_32.dll", nullptr, 0);
  ASSERT_NE(expected, nullptr);
  EXPECT_EQ(LoadLibraryW(L"nevr-rewrite-me.dll"), expected);
}
