#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "runtime/patch/asset_cache.h"
#include "runtime/patch/resource_override.h"

bool TestLogContains(const std::string& needle);
void ClearTestLogs();

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

TEST(ResourceOverride, ShutdownReportsOwnedBuffersKeptByLiveHook) {
  char directory[MAX_PATH] = {};
  char path[MAX_PATH] = {};
  ASSERT_NE(GetTempPathA(MAX_PATH, directory), 0u);
  ASSERT_NE(GetTempFileNameA(directory, "nvr", 0, path), 0u);
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "owned";
    ASSERT_TRUE(file.good());
  }

  TestHook_InitResourceOverrides(&NoopOriginal);
  RegisterResourceOverrideFromFile(kType, kName, path, "owned-test");
  ClearTestLogs();
  const ResourceOverrideShutdownReport report = ShutdownResourceOverride();
  EXPECT_EQ(report.registered, 1u);
  EXPECT_TRUE(report.hook_installed);
  EXPECT_TRUE(TestLogContains("owned_buffers_process_lifetime=1"));

  TestHook_SetOriginal(nullptr);
  ShutdownResourceOverride();
  EXPECT_NE(DeleteFileA(path), 0);
}

// A game callback that never returns used to hang plugin unload with nothing in the log. The wait is bounded
// (shortened here), reports the timeout, and leaves the buffers alone.
TEST(ResourceOverride, DeregistrationGivesUpOnABlockedCallbackAndSaysSo) {
  {
    std::lock_guard<std::mutex> lock(g_gateMutex);
    g_originalEntered = false;
    g_releaseOriginal = false;
    g_forwardedBuffer = nullptr;
  }
  TestHook_SetDeregisterWaitMs(100);
  TestHook_InitResourceOverrides(&BlockingOriginal);
  static char replacement[] = "replacement";
  RegisterResourceOverride(kType, kName, replacement, sizeof(replacement), "test");

  std::thread callbackThread([] { RunCallbackFor(kName); });
  {
    std::unique_lock<std::mutex> lock(g_gateMutex);
    ASSERT_TRUE(g_gate.wait_for(lock, 5s, [] { return g_originalEntered; }));
  }
  const unsigned long long before = TestHook_DeregisterTimeouts();
  auto deregister = std::async(std::launch::async, [&] {
    DeregisterResourceOverrides(replacement, replacement + sizeof(replacement));
  });
  EXPECT_EQ(deregister.wait_for(5s), std::future_status::ready)
      << "the bound did not release deregistration while the callback was still blocked";
  EXPECT_EQ(TestHook_DeregisterTimeouts(), before + 1);
  {
    std::lock_guard<std::mutex> lock(g_gateMutex);
    g_releaseOriginal = true;
  }
  g_gate.notify_all();
  callbackThread.join();
  TestHook_SetDeregisterWaitMs(30000);
  TestHook_CloseResourceOverrides();
}

// ── #703: overrides from the asset cache, and the label that outlives its caller ──────────────────────────────

constexpr std::uint64_t kSplashType = 0xbeac1969cb7b8861ull;
constexpr std::uint64_t kSplashName = 0x1f72c5e0fc9fc8c0ull;

const void* g_seenBuffer = nullptr;
std::uint64_t g_seenSize = 0;

void RecordingOriginal(void* callbackData) {
  const auto* bytes = static_cast<const std::uint8_t*>(callbackData);
  std::memcpy(&g_seenBuffer, bytes + 0x18, sizeof(g_seenBuffer));
  std::memcpy(&g_seenSize, bytes + 0x20, sizeof(g_seenSize));
}

// The game finishing the read of (type, name): buffer index 0, `origBuffer` of `origSize` bytes.
void GameFinishesReading(std::uint64_t type, std::uint64_t name, const void* origBuffer, std::uint64_t origSize) {
  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x28> callback{};
  void* resourcePtr = resource.data();
  std::memcpy(resource.data() + 0x28, &type, sizeof(type));
  std::memcpy(resource.data() + 0x38, &name, sizeof(name));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));
  std::memcpy(callback.data() + 0x18, &origBuffer, sizeof(origBuffer));
  std::memcpy(callback.data() + 0x20, &origSize, sizeof(origSize));
  g_seenBuffer = nullptr;
  g_seenSize = 0;
  TestHook_RunResourceCallback(callback.data());
}

class CachedOverrides : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    dir_ = std::wstring(tmp) + L"nevr-cached-overrides-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::wstring(test.begin(), test.end());
    Wipe();
    TestHook_InitResourceOverrides(&RecordingOriginal);
    ClearTestLogs();
  }
  void TearDown() override {
    TestHook_CloseResourceOverrides();
    Wipe();
  }
  void Wipe() {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir_ + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) DeleteFileW((dir_ + L"\\" + fd.cFileName).c_str());
      } while (FindNextFileW(find, &fd) != 0);
      FindClose(find);
    }
    RemoveDirectoryW(dir_.c_str());
  }
  // Puts `data` in the cache and names it in the index for (type, name).
  std::string Cache(const std::vector<std::uint8_t>& data, std::uint64_t type, std::uint64_t name, const char* label) {
    const nevr_asset_cache::StoreResult stored = nevr_asset_cache::Store(dir_, data.data(), data.size());
    EXPECT_NE(stored.status, nevr_asset_cache::StoreStatus::Failed) << stored.failedStep << " " << stored.error;
    std::string err;
    EXPECT_TRUE(nevr_asset_cache::SetIndexEntry(dir_, {type, name, stored.sha256, label}, &err)) << err;
    return stored.sha256;
  }
  static std::vector<std::uint8_t> Image(std::size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 7);
    return v;
  }
  std::wstring dir_;
};

TEST_F(CachedOverrides, ACachedFileIsDeliveredForItsTypeAndNameAndLogged) {
  const std::vector<std::uint8_t> image = Image(3000, 1);
  const std::string sha = Cache(image, kSplashType, kSplashName, "cache_splash");
  ASSERT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 1u);

  char original[10] = {};
  GameFinishesReading(kSplashType, kSplashName, original, sizeof(original));
  ASSERT_NE(g_seenBuffer, nullptr);
  EXPECT_NE(g_seenBuffer, static_cast<const void*>(original)) << "the engine must be handed the cached bytes";
  EXPECT_EQ(g_seenSize, image.size());
  EXPECT_EQ(std::memcmp(g_seenBuffer, image.data(), image.size()), 0);
  EXPECT_TRUE(TestLogContains("cache hit sha256=" + sha + " bytes=3000 type=0xbeac1969cb7b8861 name=0x1f72c5e0fc9fc8c0"));
  EXPECT_TRUE(TestLogContains("Registered file override: cache_splash ("));
  EXPECT_TRUE(TestLogContains(sha + ".blob, 3000 bytes)"));
  EXPECT_TRUE(TestLogContains("override applied label=cache_splash"));
  EXPECT_TRUE(TestLogContains("orig_bytes=10 new_bytes=3000"));
  EXPECT_TRUE(TestLogContains("cache init dir="));
  EXPECT_TRUE(TestLogContains("entries=1 loaded=1 refused=0 missing=0"));
}

TEST_F(CachedOverrides, ACachedFileForAnotherResourceIsNotDeliveredForThisOne) {
  Cache(Image(100, 1), kSplashType, kSplashName, "cache_splash");
  ASSERT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 1u);
  char original[4] = {};
  GameFinishesReading(kSplashType, kSplashName + 1, original, sizeof(original));
  EXPECT_EQ(g_seenBuffer, static_cast<const void*>(original));
  EXPECT_FALSE(TestLogContains("override applied"));
}

TEST_F(CachedOverrides, ACachedFileForAnotherTypeWithTheSameNameIsNotDelivered) {
  Cache(Image(100, 1), kSplashType, kSplashName, "cache_splash");
  ASSERT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 1u);
  char original[4] = {};
  GameFinishesReading(kSplashType + 1, kSplashName, original, sizeof(original));
  EXPECT_EQ(g_seenBuffer, static_cast<const void*>(original));
  EXPECT_FALSE(TestLogContains("override applied"));
}

TEST_F(CachedOverrides, AOneByteFlippedCachedFileIsRefusedLoggedAndNeverDelivered) {
  const std::vector<std::uint8_t> image = Image(2048, 5);
  const std::string sha = Cache(image, kSplashType, kSplashName, "cache_splash");
  std::vector<std::uint8_t> bad = image;
  bad[700] ^= 0x01;
  const std::wstring blob = nevr_asset_cache::BlobPath(dir_, sha);
  HANDLE f = CreateFileW(blob.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(f, INVALID_HANDLE_VALUE);
  DWORD put = 0;
  ASSERT_NE(WriteFile(f, bad.data(), static_cast<DWORD>(bad.size()), &put, nullptr), 0);
  CloseHandle(f);

  EXPECT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 0u);
  EXPECT_TRUE(TestLogContains("cache refused reason=sha256 mismatch expected=" + sha + " actual="));
  EXPECT_TRUE(TestLogContains("entries=1 loaded=0 refused=1 missing=0"));
  EXPECT_FALSE(TestLogContains("Registered file override"));
  char original[4] = {};
  GameFinishesReading(kSplashType, kSplashName, original, sizeof(original));
  EXPECT_EQ(g_seenBuffer, static_cast<const void*>(original));
  EXPECT_FALSE(TestLogContains("override applied"));
}

TEST_F(CachedOverrides, AnIndexedBlobThatIsNotInTheCacheIsReportedMissing) {
  const std::string sha = Cache(Image(64, 2), kSplashType, kSplashName, "cache_splash");
  ASSERT_NE(DeleteFileW(nevr_asset_cache::BlobPath(dir_, sha).c_str()), 0);
  EXPECT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 0u);
  EXPECT_TRUE(TestLogContains("cache miss reason=blob not in cache sha256=" + sha));
  EXPECT_TRUE(TestLogContains("missing=1"));
}

TEST_F(CachedOverrides, NoCacheDirectoryNoIndexAndABrokenIndexEachSayWhy) {
  EXPECT_EQ(RegisterCachedResourceOverrides(nullptr), 0u);
  EXPECT_TRUE(TestLogContains("cache disabled reason=no cache directory"));
  ClearTestLogs();
  EXPECT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 0u);
  EXPECT_TRUE(TestLogContains("reason=no index.json"));
  ClearTestLogs();
  ASSERT_NE(CreateDirectoryW(dir_.c_str(), nullptr), 0);
  HANDLE f = CreateFileW((dir_ + L"\\index.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(f, INVALID_HANDLE_VALUE);
  DWORD put = 0;
  WriteFile(f, "{ not json", 10, &put, nullptr);
  CloseHandle(f);
  EXPECT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 0u);
  EXPECT_TRUE(TestLogContains("cache index unusable"));
}

TEST_F(CachedOverrides, ACachedSplashReplacesTheBuiltInOneAndTheBuiltInIsNotLeftUnapplied) {
  const std::vector<std::uint8_t> image = Image(500, 9);
  Cache(image, kSplashType, kSplashName, "cache_splash");
  TestHook_RegisterInitialOverrides(dir_.c_str());
  EXPECT_TRUE(TestLogContains("built-in splash_texture not registered reason=a cached override covers it"));
  EXPECT_FALSE(TestLogContains("Registered embedded override: splash_texture"));
  char original[4] = {};
  GameFinishesReading(kSplashType, kSplashName, original, sizeof(original));
  EXPECT_EQ(g_seenSize, image.size());
  EXPECT_TRUE(TestLogContains("override applied label=cache_splash"));
  const ResourceOverrideShutdownReport report = ShutdownResourceOverride();
  EXPECT_EQ(report.registered, 1u);
  EXPECT_EQ(report.never_applied, 0u);
}

TEST_F(CachedOverrides, ACachedOverrideOfAnotherTypeDoesNotSuppressTheBuiltInSplash) {
  Cache(Image(90, 3), kSplashType + 1, kSplashName, "other_type");
  TestHook_RegisterInitialOverrides(dir_.c_str());
  EXPECT_TRUE(TestLogContains("Registered embedded override: splash_texture"));
  EXPECT_FALSE(TestLogContains("not registered reason"));
}

TEST_F(CachedOverrides, WithNothingCachedTheBuiltInSplashIsRegistered) {
  TestHook_RegisterInitialOverrides(dir_.c_str());
  EXPECT_TRUE(TestLogContains("Registered embedded override: splash_texture"));
  EXPECT_FALSE(TestLogContains("not registered reason"));
}

TEST_F(CachedOverrides, TheCacheSizeIsLoggedEveryRunAndAnOverCapStateIsAWarning) {
  Cache(Image(100, 1), kSplashType, kSplashName, "cache_splash");
  RegisterCachedResourceOverrides(dir_.c_str());
  EXPECT_TRUE(TestLogContains("cache size blob_bytes_before=100 blob_bytes_after=100 cap="));
  EXPECT_TRUE(TestLogContains("over_cap_all_referenced=false"));
}

// The label used to be kept as a raw pointer to the caller's buffer and printed when the game later loaded the
// resource: for a file found by the _overrides/ scan that buffer was a stack struct long gone (#703).
TEST_F(CachedOverrides, TheLabelIsCopiedSoACallersBufferMayBeReusedBeforeTheGameLoadsTheResource) {
  char path[MAX_PATH] = {};
  char directory[MAX_PATH] = {};
  ASSERT_NE(GetTempPathA(MAX_PATH, directory), 0u);
  ASSERT_NE(GetTempFileNameA(directory, "nvl", 0, path), 0u);
  HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(f, INVALID_HANDLE_VALUE);
  DWORD put = 0;
  WriteFile(f, "file-bytes", 10, &put, nullptr);
  CloseHandle(f);

  char fileLabel[32] = "label-from-a-stack-buffer";
  RegisterResourceOverrideFromFile(kType, kName, path, fileLabel);
  char embeddedLabel[32] = "embedded-stack-label";
  static char embeddedBytes[] = "emb";
  RegisterResourceOverride(kType, kName + 1, embeddedBytes, sizeof(embeddedBytes), embeddedLabel);
  std::memset(fileLabel, 'X', sizeof(fileLabel) - 1);  // the caller's buffer is reused
  std::memset(embeddedLabel, 'Y', sizeof(embeddedLabel) - 1);

  char original[4] = {};
  GameFinishesReading(kType, kName, original, sizeof(original));
  GameFinishesReading(kType, kName + 1, original, sizeof(original));
  EXPECT_TRUE(TestLogContains("override applied label=label-from-a-stack-buffer "));
  EXPECT_TRUE(TestLogContains("override applied label=embedded-stack-label "));
  EXPECT_FALSE(TestLogContains("XXXX"));
  EXPECT_FALSE(TestLogContains("YYYY"));
  TestHook_SetOriginal(nullptr);
  ShutdownResourceOverride();
  EXPECT_NE(DeleteFileA(path), 0);
}

TEST_F(CachedOverrides, TheSizeCapEvictsAStrayBlobAndNeverOneTheIndexNames) {
  const std::vector<std::uint8_t> kept = Image(4000, 3);
  const std::string keptSha = Cache(kept, kSplashType, kSplashName, "cache_splash");
  const std::vector<std::uint8_t> strayBytes = Image(4000, 4);
  const nevr_asset_cache::StoreResult stray = nevr_asset_cache::Store(dir_, strayBytes.data(), strayBytes.size());
  ASSERT_EQ(stray.status, nevr_asset_cache::StoreStatus::Stored);
  EXPECT_EQ(RegisterCachedResourceOverrides(dir_.c_str(), 1000), 1u);
  EXPECT_NE(GetFileAttributesW(nevr_asset_cache::BlobPath(dir_, keptSha).c_str()), INVALID_FILE_ATTRIBUTES)
      << "a blob the index names is in use";
  EXPECT_EQ(GetFileAttributesW(nevr_asset_cache::BlobPath(dir_, stray.sha256).c_str()), INVALID_FILE_ATTRIBUTES)
      << "an unreferenced blob over the cap goes";
  EXPECT_TRUE(TestLogContains("cache size blob_bytes_before=8000 blob_bytes_after=4000 cap=1000 evicted=1"));
  EXPECT_TRUE(TestLogContains("over_cap_all_referenced=true"));
}

TEST_F(CachedOverrides, ABlobOverTheSizeLimitIsRefusedWithItsReason) {
  const std::string sha(64, 'c');
  std::string err;
  ASSERT_NE(CreateDirectoryW(dir_.c_str(), nullptr), 0);
  ASSERT_TRUE(nevr_asset_cache::SetIndexEntry(dir_, {kSplashType, kSplashName, sha, "huge"}, &err)) << err;
  HANDLE f = CreateFileW(nevr_asset_cache::BlobPath(dir_, sha).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(f, INVALID_HANDLE_VALUE);
  LARGE_INTEGER end;
  end.QuadPart = static_cast<LONGLONG>(nevr_asset_cache::kMaxBlobBytes) + 1;
  ASSERT_NE(SetFilePointerEx(f, end, nullptr, FILE_BEGIN), 0);
  ASSERT_NE(SetEndOfFile(f), 0);
  CloseHandle(f);
  EXPECT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 0u);
  EXPECT_TRUE(TestLogContains("cache refused reason=blob over the size limit sha256=" + sha));
  EXPECT_TRUE(TestLogContains("entries=1 loaded=0 refused=1 missing=0"));
}

TEST_F(CachedOverrides, ACachedOverrideIsAnOwnedBufferTheShutdownReportCounts) {
  Cache(Image(120, 8), kSplashType, kSplashName, "cache_splash");
  ASSERT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 1u);
  ClearTestLogs();
  const ResourceOverrideShutdownReport report = ShutdownResourceOverride();
  EXPECT_EQ(report.registered, 1u);
  EXPECT_TRUE(TestLogContains("owned_buffers_process_lifetime=1"));
}

TEST_F(CachedOverrides, AnEntryWithoutALabelIsNamedAfterItsHash) {
  const std::string sha = Cache(Image(80, 6), kSplashType, kSplashName, "");
  EXPECT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 1u);
  EXPECT_TRUE(TestLogContains("Registered file override: cache:" + sha.substr(0, 12) + " ("));
}

// ── Transforms: the game's buffer in, the served bytes out, on every load ────────────────────────────────────

constexpr std::uint64_t kTransformType = 0x4a3fe67c1ff9ccc4ull;
constexpr std::uint64_t kTransformName = 0xac360e41e4ede056ull;

struct TransformProbe {
  int calls = 0;
  const void* seenIn = nullptr;
  std::uint64_t seenSize = 0;
  void* seenUser = nullptr;
  bool answer = true;
  bool throws = false;
  std::vector<std::uint8_t> served = {9, 8, 7, 6, 5};
};
TransformProbe g_probe;

bool ProbeTransform(const void* in, std::uint64_t inSize, const void** out, std::uint64_t* outSize, void* user) {
  ++g_probe.calls;
  g_probe.seenIn = in;
  g_probe.seenSize = inSize;
  g_probe.seenUser = user;
  if (g_probe.throws) throw std::runtime_error("transform failure");
  if (!g_probe.answer) return false;
  *out = g_probe.served.data();
  *outSize = g_probe.served.size();
  return true;
}

// The game finishing buffer `index` of (type, name).
void GameFinishesBuffer(std::uint64_t type, std::uint64_t name, std::uint32_t index, const void* buffer,
                        std::uint64_t size) {
  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x28> callback{};
  void* resourcePtr = resource.data();
  std::memcpy(resource.data() + 0x28, &type, sizeof(type));
  std::memcpy(resource.data() + 0x38, &name, sizeof(name));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));
  std::memcpy(callback.data() + 0x10, &index, sizeof(index));
  std::memcpy(callback.data() + 0x18, &buffer, sizeof(buffer));
  std::memcpy(callback.data() + 0x20, &size, sizeof(size));
  g_seenBuffer = nullptr;
  g_seenSize = 0;
  TestHook_RunResourceCallback(callback.data());
}

class ResourceTransforms : public CachedOverrides {
 protected:
  void SetUp() override {
    CachedOverrides::SetUp();
    g_probe = TransformProbe();
  }
};

TEST_F(ResourceTransforms, TheTransformSeesTheGamesBufferAndItsAnswerIsWhatTheGameReads) {
  int token = 0;
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, &token, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_probe.calls, 1);
  EXPECT_EQ(g_probe.seenIn, stock);
  EXPECT_EQ(g_probe.seenSize, sizeof(stock));
  EXPECT_EQ(g_probe.seenUser, &token);
  EXPECT_EQ(g_seenBuffer, g_probe.served.data());
  EXPECT_EQ(g_seenSize, g_probe.served.size());
  EXPECT_TRUE(TestLogContains("transform applied label=probe"));
}

TEST_F(ResourceTransforms, EveryLoadOfTheResourceIsAnswered) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  for (int load = 1; load <= 3; ++load) {
    GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
    EXPECT_EQ(g_probe.calls, load);
    EXPECT_EQ(g_seenBuffer, g_probe.served.data()) << "load " << load;
  }
}

TEST_F(ResourceTransforms, ATransformThatDeclinesLeavesTheGamesBufferAlone) {
  g_probe.answer = false;
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_probe.calls, 1);
  EXPECT_EQ(g_seenBuffer, stock);
  EXPECT_EQ(g_seenSize, sizeof(stock));
}

TEST_F(ResourceTransforms, ATransformThatThrowsKeepsTheGamesBufferAndSaysSo) {
  g_probe.throws = true;
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_seenBuffer, stock);
  EXPECT_EQ(g_seenSize, sizeof(stock));
  EXPECT_TRUE(TestLogContains("transform threw label=probe"));
  EXPECT_TRUE(TestLogContains("what=transform failure"));
}

TEST_F(ResourceTransforms, OnlyThePrimaryBufferOfTheNamedResourceIsOffered) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 1, stock, sizeof(stock));      // the secondary buffer
  GameFinishesBuffer(kTransformType, kTransformName + 1, 0, stock, sizeof(stock));  // another name
  GameFinishesBuffer(kTransformType + 1, kTransformName, 0, stock, sizeof(stock));  // another type
  EXPECT_EQ(g_probe.calls, 0);
  EXPECT_EQ(g_seenBuffer, stock);
}

TEST_F(ResourceTransforms, AnEmptyAnswerIsNotServed) {
  g_probe.served.clear();
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_probe.calls, 1);
  EXPECT_EQ(g_seenBuffer, stock);
}

TEST_F(ResourceTransforms, ARequestedTransformIsNotReportedAsNeverApplied) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  RegisterResourceTransform(kTransformType, kTransformName + 5, &ProbeTransform, nullptr, "idle");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ClearTestLogs();
  const ResourceOverrideShutdownReport report = ShutdownResourceOverride();
  EXPECT_EQ(report.registered, 2u);
  EXPECT_EQ(report.never_applied, 1u);
  EXPECT_TRUE(TestLogContains("override never applied label=idle"));
}

TEST_F(ResourceTransforms, ANullTransformRegistersNothing) {
  RegisterResourceTransform(kTransformType, kTransformName, nullptr, nullptr, "none");
  EXPECT_EQ(ShutdownResourceOverride().registered, 0u);
}

}  // namespace
