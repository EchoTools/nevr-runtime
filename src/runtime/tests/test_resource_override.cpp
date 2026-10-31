#include <gtest/gtest.h>
#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

#include "core/logging.h"
#include "runtime/patch/asset_cache.h"
#include "runtime/patch/game_buffer.h"
#include "runtime/patch/netgun_bullets.h"
#include "runtime/patch/resource_override.h"
#include "runtime/tests/netgun_bullets_fixture.h"
#include "runtime/lifecycle/cli.h"
#include "core/logging.h"

bool TestLogContains(const std::string& needle);
bool TestLogContainsAtLevel(EchoVR::LogLevel level, const std::string& needle);
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

// ── #1146: the game's own heap, as the resource release sees it ───────────────────────────────────────────────
// The game frees buffer 0 of a resource through the allocator at memory-context + 0x10, slot kFreeSlot, and never checks
// where the pointer came from. This fake records what its alloc slot hands out; its free slot counts any pointer it
// did not hand out as foreign, which in the game is a free into the Resource pool of memory it does not own.
struct FakeHeap {
  struct Block {
    std::unique_ptr<std::uint8_t[]> memory;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
    bool freed = false;
  };
  std::mutex mutex;
  std::vector<Block> blocks;
  unsigned foreignFrees = 0;
  bool failAllocations = false;

  const Block* Find(const void* p) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const Block& b : blocks) {
      if (b.memory.get() == p) return &b;
    }
    return nullptr;
  }
};
FakeHeap g_heap;

struct FakeAllocator {
  void* const* table;
};
void* FakeAlloc(void* /*allocator*/, std::uint64_t alignment, std::uint64_t size) {
  std::lock_guard<std::mutex> lock(g_heap.mutex);
  if (g_heap.failAllocations) return nullptr;
  FakeHeap::Block block;
  block.memory.reset(new std::uint8_t[static_cast<std::size_t>(size)]);
  block.size = size;
  block.alignment = alignment;
  void* const p = block.memory.get();
  g_heap.blocks.push_back(std::move(block));
  return p;
}
std::uint64_t FakeFree(void* /*allocator*/, void* pointer) {
  std::lock_guard<std::mutex> lock(g_heap.mutex);
  for (FakeHeap::Block& b : g_heap.blocks) {
    if (b.memory.get() == pointer && !b.freed) {
      b.freed = true;
      return 0;
    }
  }
  ++g_heap.foreignFrees;
  return 1;
}
void* g_fakeTable[8] = {};
FakeAllocator g_fakeAllocator{g_fakeTable};
std::uint8_t g_fakeContext[0x20] = {};
void* g_fakeContextPointer = nullptr;

void InstallFakeHeap() {
  g_fakeTable[5]  /* byte offset 0x28: CElephantAllocator::vslot[5] */ = reinterpret_cast<void*>(&FakeAlloc);
  g_fakeTable[7]  /* byte offset 0x38: CElephantAllocator::vslot[7] */ = reinterpret_cast<void*>(&FakeFree);
  FakeAllocator* const allocator = &g_fakeAllocator;
  std::memcpy(g_fakeContext + 0x10 /* memory context + 0x10 */, &allocator, sizeof(allocator));
  g_fakeContextPointer = g_fakeContext;
  {
    std::lock_guard<std::mutex> lock(g_heap.mutex);
    g_heap.blocks.clear();
    g_heap.foreignFrees = 0;
    g_heap.failAllocations = false;
  }
  TestHook_SetMemoryContextSlot(&g_fakeContextPointer);
}
void RemoveFakeHeap() { TestHook_SetMemoryContextSlot(nullptr); }

// The allocator and alignment the game's reader records in the callback block before it allocates (SetupReadBuffer).
void SeedBlock(std::uint8_t* block) {
  FakeAllocator* const allocator = &g_fakeAllocator;
  const std::uint64_t alignment = 16;
  std::memcpy(block + 0x28, &allocator, sizeof(allocator));
  std::memcpy(block + 0x30, &alignment, sizeof(alignment));
}

// The game unloads the resource: it frees the pointer it holds in the block.
void GameFreesBuffer(const void* pointer) {
  using FreeFn = std::uint64_t (*)(void*, void*);
  reinterpret_cast<FreeFn>(g_fakeTable[7]  /* byte offset 0x38: CElephantAllocator::vslot[7] */)(&g_fakeAllocator,
                                                                                    const_cast<void*>(pointer));
}

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

char g_selfReplacement[] = "replacement";

// The callback deregisters the replacement it is being served. The game holds a copy now (#1146), so the registry's own
// bytes are named directly.
void SelfDeregisteringOriginal(void* /*callbackData*/) {
  DeregisterResourceOverrides(g_selfReplacement, g_selfReplacement + sizeof(g_selfReplacement));
}

void RunSelfDeregisterFixture() {
  TestHook_InitResourceOverrides(&SelfDeregisteringOriginal);
  InstallFakeHeap();
  RegisterResourceOverride(kType, kName, g_selfReplacement, sizeof(g_selfReplacement), "test");

  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x38> callback{};
  SeedBlock(callback.data());
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
  InstallFakeHeap();
  char replacement[] = "replacement";
  RegisterResourceOverride(kType, kName, replacement, sizeof(replacement), "test");

  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x38> callback{};
  SeedBlock(callback.data());
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
  // The game is handed a copy from its own heap, never the registry's bytes (#1146).
  EXPECT_NE(g_forwardedBuffer, replacement);
  ASSERT_NE(g_forwardedBuffer, nullptr);
  EXPECT_NE(g_heap.Find(g_forwardedBuffer), nullptr);
  EXPECT_EQ(std::memcmp(g_forwardedBuffer, replacement, sizeof(replacement)), 0);
  TestHook_CloseResourceOverrides();
  RemoveFakeHeap();
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
  std::array<std::uint8_t, 0x38> callback{};
  SeedBlock(callback.data());
  void* resourcePtr = resource.data();
  std::memcpy(resource.data() + 0x28, &kType, sizeof(kType));
  std::memcpy(resource.data() + 0x38, &name, sizeof(name));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));
  TestHook_RunResourceCallback(callback.data());
}

TEST(ResourceOverride, ShutdownReportsEveryOverrideTheGameNeverRequested) {
  TestHook_InitResourceOverrides(&NoopOriginal);
  InstallFakeHeap();
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
  RemoveFakeHeap();
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
  InstallFakeHeap();
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
  RemoveFakeHeap();
}

// ── #703: overrides from the asset cache, and the label that outlives its caller ──────────────────────────────

// These cache tests give the resource this key as its type at CResource + 0x28 and finish buffer 0: an entry equal to
// the logical type names buffer 0 (resource_buffer_key.h), so they drive the cache path, not a texture's payload.
// The payload path, with the real logical type, is TexturePayloads below.
constexpr std::uint64_t kSplashType = 0xbeac1969cb7b8861ull;
constexpr std::uint64_t kSplashName = 0x1f72c5e0fc9fc8c0ull;

const void* g_seenBuffer = nullptr;
std::uint64_t g_seenSize = 0;

void RecordingOriginal(void* callbackData) {
  const auto* bytes = static_cast<const std::uint8_t*>(callbackData);
  std::memcpy(&g_seenBuffer, bytes + 0x18, sizeof(g_seenBuffer));
  std::memcpy(&g_seenSize, bytes + 0x20, sizeof(g_seenSize));
}

// What the engine must be holding after a replacement: bytes equal to `want`, in a block its own allocator handed out,
// never the caller's pointer (#1146).
void ExpectGameCopy(const void* want, std::size_t n) {
  ASSERT_NE(g_seenBuffer, nullptr);
  EXPECT_NE(g_seenBuffer, want) << "the engine was handed the registry's own bytes";
  const FakeHeap::Block* block = g_heap.Find(g_seenBuffer);
  ASSERT_NE(block, nullptr) << "the engine was handed memory its allocator did not allocate";
  EXPECT_EQ(block->size, n);
  EXPECT_GE(block->alignment, 16u);
  EXPECT_EQ(g_seenSize, n);
  EXPECT_EQ(std::memcmp(g_seenBuffer, want, n), 0);
}

// The game finishing the read of (type, name): buffer index 0, `origBuffer` of `origSize` bytes.
void GameFinishesReading(std::uint64_t type, std::uint64_t name, const void* origBuffer, std::uint64_t origSize) {
  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x38> callback{};
  SeedBlock(callback.data());
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
    // test_behavioral.cpp defaults g_isServer to TRUE for its binary; the splash is a client's, so these tests run as one.
    wasServer_ = g_isServer;
    g_isServer = FALSE;
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    dir_ = std::wstring(tmp) + L"nevr-cached-overrides-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::wstring(test.begin(), test.end());
    Wipe();
    TestHook_InitResourceOverrides(&RecordingOriginal);
    InstallFakeHeap();
    ClearTestLogs();
  }
  void TearDown() override {
    TestHook_CloseResourceOverrides();
    RemoveFakeHeap();
    Wipe();
    g_isServer = wasServer_;
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
  BOOL wasServer_ = FALSE;
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
  ExpectGameCopy(image.data(), image.size());
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

constexpr std::uint64_t kTransformType = 0x75e5993f80940cd0ull;
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
                        std::uint64_t size, const void* blockAllocator = &g_fakeAllocator) {
  std::array<std::uint8_t, 0x40> resource{};
  std::array<std::uint8_t, 0x38> callback{};
  SeedBlock(callback.data());
  void* resourcePtr = resource.data();
  std::memcpy(resource.data() + 0x28, &type, sizeof(type));
  std::memcpy(resource.data() + 0x38, &name, sizeof(name));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));
  std::memcpy(callback.data() + 0x10, &index, sizeof(index));
  std::memcpy(callback.data() + 0x18, &buffer, sizeof(buffer));
  std::memcpy(callback.data() + 0x20, &size, sizeof(size));
  std::memcpy(callback.data() + 0x28, &blockAllocator, sizeof(blockAllocator));
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
  ExpectGameCopy(g_probe.served.data(), g_probe.served.size());
  EXPECT_TRUE(TestLogContains("transform applied label=probe"));
}

TEST_F(ResourceTransforms, EveryLoadOfTheResourceIsAnswered) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  for (int load = 1; load <= 3; ++load) {
    GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
    EXPECT_EQ(g_probe.calls, load);
    ExpectGameCopy(g_probe.served.data(), g_probe.served.size());
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

TEST_F(ResourceTransforms, ATransformWhoseResourceNeverLoadedIsNotAWarning) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  RegisterResourceTransform(kTransformType, kTransformName + 5, &ProbeTransform, nullptr, "idle");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ClearTestLogs();
  const ResourceOverrideShutdownReport report = ShutdownResourceOverride();
  EXPECT_EQ(report.registered, 2u);
  EXPECT_EQ(report.never_applied, 0u) << "a transform is not a fixed override, so it is not counted as one";
  EXPECT_EQ(report.transforms_unserved, 1u);
  EXPECT_FALSE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning, "transform not served label=idle"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Debug, "transform not served label=idle"));
  EXPECT_FALSE(TestLogContains("override never applied label=idle"));
}

TEST_F(ResourceTransforms, ANullTransformRegistersNothing) {
  EXPECT_FALSE(RegisterResourceTransform(kTransformType, kTransformName, nullptr, nullptr, "none"));
  EXPECT_EQ(ShutdownResourceOverride().registered, 0u);
}

TEST_F(ResourceTransforms, ARegisteredTransformReportsItRegistered) {
  EXPECT_TRUE(RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe"));
  EXPECT_EQ(ShutdownResourceOverride().registered, 1u);
}

// ── #1146: what reaches the game is the game's own memory ─────────────────────────────────────────────────────

// ── #1479: a plugin's override of a resource outranks a core transform of it, whatever the registration order ──

TEST_F(ResourceTransforms, APluginOverrideOutranksACoreTransformRegisteredBeforeIt) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "core-transform");
  static const std::uint8_t plugin[] = {10, 20, 30, 40};
  RegisterResourceOverride(kTransformType, kTransformName, plugin, sizeof(plugin), "plugin-override");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ExpectGameCopy(plugin, sizeof(plugin));
  EXPECT_EQ(g_probe.calls, 0);
  EXPECT_TRUE(TestLogContains(
      "override displaces transform type=0x75e5993f80940cd0 name=0xac360e41e4ede056 override=plugin-override "
      "transform=core-transform"));
}

TEST_F(ResourceTransforms, APluginOverrideOutranksACoreTransformRegisteredAfterIt) {
  static const std::uint8_t plugin[] = {10, 20, 30, 40};
  RegisterResourceOverride(kTransformType, kTransformName, plugin, sizeof(plugin), "plugin-override");
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "core-transform");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ExpectGameCopy(plugin, sizeof(plugin));
  EXPECT_EQ(g_probe.calls, 0);
  EXPECT_TRUE(TestLogContains(
      "override displaces transform type=0x75e5993f80940cd0 name=0xac360e41e4ede056 override=plugin-override "
      "transform=core-transform"));
}

TEST_F(ResourceTransforms, AmongFixedOverridesOfOneKeyTheFirstRegisteredWins) {
  static const std::uint8_t first[] = {10, 20, 30, 40};
  static const std::uint8_t second[] = {50, 60, 70, 80};
  RegisterResourceOverride(kTransformType, kTransformName, first, sizeof(first), "first");
  RegisterResourceOverride(kTransformType, kTransformName, second, sizeof(second), "second");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ExpectGameCopy(first, sizeof(first));
}

TEST_F(ResourceTransforms, AmongTransformsOfOneKeyTheFirstRegisteredAnswers) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "first-transform");
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "second-transform");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_probe.calls, 1);
  EXPECT_TRUE(TestLogContains("transform applied label=first-transform"));
  EXPECT_FALSE(TestLogContains("transform applied label=second-transform"));
}

TEST_F(ResourceTransforms, ACoreTransformAloneStillAppliesAndDisplacesNothing) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "core-transform");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_probe.calls, 1);
  ExpectGameCopy(g_probe.served.data(), g_probe.served.size());
  EXPECT_FALSE(TestLogContains("displaces transform"));
}

bool DecliningTransform(const void* /*in*/, std::uint64_t /*inSize*/, const void** /*out*/, std::uint64_t* /*outSize*/,
                        void* /*user*/) {
  return false;
}

TEST_F(ResourceTransforms, ATransformThatDeclinesHandsTheLoadToTheNextTransform) {
  RegisterResourceTransform(kTransformType, kTransformName, &DecliningTransform, nullptr, "declines");
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "answers");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_probe.calls, 1);
  ExpectGameCopy(g_probe.served.data(), g_probe.served.size());
  EXPECT_TRUE(TestLogContains("transform kept the game's buffer label=declines"));
  EXPECT_TRUE(TestLogContains("transform applied label=answers"));
}

TEST_F(ResourceTransforms, AFixedOverridePassedOverByAnEarlierOneIsReportedAsSuch) {
  static const std::uint8_t first[] = {10, 20, 30, 40};
  static const std::uint8_t second[] = {50, 60, 70, 80};
  RegisterResourceOverride(kTransformType, kTransformName, first, sizeof(first), "first");
  RegisterResourceOverride(kTransformType, kTransformName, second, sizeof(second), "second");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ClearTestLogs();
  const ResourceOverrideShutdownReport report = ShutdownResourceOverride();
  EXPECT_EQ(report.never_applied, 1u);
  EXPECT_TRUE(TestLogContains("override passed over label=second"));
  EXPECT_FALSE(TestLogContains("override never applied label=second"));
}

class GameHeapCopies : public ResourceTransforms {};

TEST_F(GameHeapCopies, AFixedReplacementIsACopyTheGameCanFreeThroughItsOwnAllocator) {
  static const std::uint8_t replacement[] = {10, 20, 30, 40, 50, 60};
  RegisterResourceOverride(kTransformType, kTransformName, replacement, sizeof(replacement), "fixed");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ExpectGameCopy(replacement, sizeof(replacement));
  EXPECT_TRUE(TestLogContains("override applied label=fixed"));

  const void* const held = g_seenBuffer;  // the pointer the game keeps in the resource
  GameFreesBuffer(held);                  // the last reference goes
  EXPECT_EQ(g_heap.foreignFrees, 0u) << "the game freed memory its allocator did not hand out";
  EXPECT_EQ(replacement[0], 10) << "the registry's own bytes are untouched by the game's free";
}

TEST_F(GameHeapCopies, ATransformAnswerIsACopyTheGameCanFreeThroughItsOwnAllocator) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ExpectGameCopy(g_probe.served.data(), g_probe.served.size());
  GameFreesBuffer(g_seenBuffer);
  EXPECT_EQ(g_heap.foreignFrees, 0u);
  EXPECT_EQ(g_probe.served[0], 9) << "the transform's own answer is untouched by the game's free";
}

// A plugin asset has no callback block (plugin_assets.cpp): its bytes reach the engine as a copy from the release allocator.
class PluginAssetCopies : public CachedOverrides {};

TEST_F(PluginAssetCopies, ACopyOfTheBytesFromTheReleaseAllocatorIsFreeableByTheGame) {
  static const std::uint8_t asset[] = {7, 8, 9, 10};
  void* copy = nullptr;
  ASSERT_EQ(nevr_game_buffer::CopyIntoGameHeapFor(&g_fakeContextPointer, asset, sizeof(asset), &copy),
            nevr_game_buffer::Copy::Copied);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy, static_cast<const void*>(asset)) << "the engine must not receive the plugin's own buffer";
  const FakeHeap::Block* block = g_heap.Find(copy);
  ASSERT_NE(block, nullptr) << "the copy was not allocated by the game's allocator";
  EXPECT_EQ(block->alignment, 16u) << "no block names an alignment, so the allocator's minimum is used";
  EXPECT_EQ(std::memcmp(copy, asset, sizeof(asset)), 0);
  GameFreesBuffer(copy);
  EXPECT_EQ(g_heap.foreignFrees, 0u) << "the game freed memory its allocator did not hand out";
}

TEST_F(PluginAssetCopies, WithNoMemoryContextNothingIsCopied) {
  static const std::uint8_t asset[] = {1, 2};
  void* copy = nullptr;
  EXPECT_EQ(nevr_game_buffer::CopyIntoGameHeapFor(nullptr, asset, sizeof(asset), &copy),
            nevr_game_buffer::Copy::NoContext);
  EXPECT_EQ(copy, nullptr);
  EXPECT_TRUE(g_heap.blocks.empty());
}

TEST_F(PluginAssetCopies, AFailedAllocationIsReportedAndNothingIsHanded) {
  static const std::uint8_t asset[] = {1, 2};
  g_heap.failAllocations = true;
  void* copy = nullptr;
  EXPECT_EQ(nevr_game_buffer::CopyIntoGameHeapFor(&g_fakeContextPointer, asset, sizeof(asset), &copy),
            nevr_game_buffer::Copy::AllocFailed);
  EXPECT_EQ(copy, nullptr);
}

// The fake must be able to see the bug it guards against: freeing the transform's own answer is a foreign free.
TEST_F(GameHeapCopies, TheFakeAllocatorCountsAFreeOfMemoryItDidNotHandOut) {
  GameFreesBuffer(g_probe.served.data());
  EXPECT_EQ(g_heap.foreignFrees, 1u);
}

TEST_F(GameHeapCopies, EveryLoadGetsItsOwnCopyAndEachCanBeFreed) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  std::vector<const void*> held;
  for (int load = 0; load < 3; ++load) {
    GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
    ExpectGameCopy(g_probe.served.data(), g_probe.served.size());
    held.push_back(g_seenBuffer);
  }
  EXPECT_NE(held[0], held[1]);
  EXPECT_NE(held[1], held[2]);
  for (const void* p : held) GameFreesBuffer(p);
  EXPECT_EQ(g_heap.foreignFrees, 0u);
}

TEST_F(GameHeapCopies, WithNoMemoryContextTheGamesBufferIsKeptAndTheLineSaysWhy) {
  RemoveFakeHeap();
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_seenBuffer, stock);
  EXPECT_EQ(g_seenSize, sizeof(stock));
  EXPECT_TRUE(TestLogContains("game-heap copy refused label=probe"));
  EXPECT_TRUE(TestLogContains("reason=no_memory_context"));
  EXPECT_FALSE(TestLogContains("transform applied"));
}

TEST_F(GameHeapCopies, AFixedReplacementRefusedForWantOfAHeapIsOfferedAgainOnTheNextLoad) {
  static const std::uint8_t replacement[] = {10, 20, 30};
  RegisterResourceOverride(kTransformType, kTransformName, replacement, sizeof(replacement), "fixed");
  static const std::uint8_t stock[] = {1, 2, 3};
  RemoveFakeHeap();
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_seenBuffer, stock);
  EXPECT_TRUE(TestLogContains("reason=no_memory_context"));
  InstallFakeHeap();
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  ExpectGameCopy(replacement, sizeof(replacement));
}

TEST_F(GameHeapCopies, AFailedAllocationKeepsTheGamesBufferAndSaysSo) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  g_heap.failAllocations = true;
  static const std::uint8_t stock[] = {1, 2, 3};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock));
  EXPECT_EQ(g_seenBuffer, stock);
  EXPECT_TRUE(TestLogContains("reason=alloc_failed"));
}

TEST_F(GameHeapCopies, ABufferFromAnotherAllocatorIsNotReplaced) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  const FakeAllocator other{g_fakeTable};
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock), &other);
  EXPECT_EQ(g_seenBuffer, stock);
  EXPECT_TRUE(TestLogContains("reason=allocator_mismatch"));
  GameFinishesBuffer(kTransformType, kTransformName, 0, stock, sizeof(stock), nullptr);
  EXPECT_EQ(g_seenBuffer, stock);
  EXPECT_TRUE(TestLogContains("reason=no_allocator"));
  EXPECT_TRUE(g_heap.blocks.empty()) << "nothing was allocated for a block that was refused";
}

TEST_F(GameHeapCopies, TheAlignmentTheGameAskedForIsPassedOn) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  std::array<std::uint8_t, 0x38> callback{};
  SeedBlock(callback.data());
  const std::uint64_t alignment = 256;
  std::memcpy(callback.data() + 0x30, &alignment, sizeof(alignment));
  std::array<std::uint8_t, 0x40> resource{};
  void* resourcePtr = resource.data();
  std::uint64_t type = kTransformType, name = kTransformName, size = sizeof(stock);
  const void* buffer = stock;
  std::memcpy(resource.data() + 0x28, &type, sizeof(type));
  std::memcpy(resource.data() + 0x38, &name, sizeof(name));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));
  std::memcpy(callback.data() + 0x18, &buffer, sizeof(buffer));
  std::memcpy(callback.data() + 0x20, &size, sizeof(size));
  TestHook_RunResourceCallback(callback.data());
  ASSERT_NE(g_seenBuffer, nullptr);
  const FakeHeap::Block* block = g_heap.Find(g_seenBuffer);
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(block->alignment, 256u);
}

TEST_F(GameHeapCopies, AnAlignmentBelowTheAllocatorsMinimumIsRaisedToIt) {
  RegisterResourceTransform(kTransformType, kTransformName, &ProbeTransform, nullptr, "probe");
  static const std::uint8_t stock[] = {1, 2, 3};
  std::array<std::uint8_t, 0x38> callback{};
  SeedBlock(callback.data());
  const std::uint64_t alignment = 0;
  std::memcpy(callback.data() + 0x30, &alignment, sizeof(alignment));
  std::array<std::uint8_t, 0x40> resource{};
  void* resourcePtr = resource.data();
  std::uint64_t type = kTransformType, name = kTransformName, size = sizeof(stock);
  const void* buffer = stock;
  std::memcpy(resource.data() + 0x28, &type, sizeof(type));
  std::memcpy(resource.data() + 0x38, &name, sizeof(name));
  std::memcpy(callback.data() + 0x08, &resourcePtr, sizeof(resourcePtr));
  std::memcpy(callback.data() + 0x18, &buffer, sizeof(buffer));
  std::memcpy(callback.data() + 0x20, &size, sizeof(size));
  TestHook_RunResourceCallback(callback.data());
  const FakeHeap::Block* block = g_heap.Find(g_seenBuffer);
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(block->alignment, 16u);
}


// ── #1421: a texture's payload (buffer 1), named by its on-disk type ─────────────────────────────────────────────
// A CGTextureResource reaches the game as two reads under one (type, name): buffer 0, a 256-byte descriptor read by the
// on-disk type Make(CGTextureResource, "Win10"), and buffer 1, the pixel payload, read by Make(CGTextureResource,
// "Win10GPU") (docs/engine/asset-system.md; both observed for the lobby posters in #1421's step 1). An entry keyed by
// an on-disk type names that buffer. The payload is written over the game's own buffer when it fits, so no payload-sized
// allocation is made in the game's heap; a payload larger than the game's buffer is not served, and a Warning says so.

constexpr std::uint64_t kTextureType = 0xb3ebe9e0bf0eb81full;         // CGTextureResource, at CResource + 0x28
constexpr std::uint64_t kTextureDescriptorKey = 0x4a4c32c49300b8a0ull;  // Make(CGTextureResource, "Win10"): buffer 0
constexpr std::uint64_t kTexturePayloadKey = 0xbeac1969cb7b8861ull;     // Make(CGTextureResource, "Win10GPU"): buffer 1
constexpr std::uint64_t kPosterName = 0xa240a2bc051b2f23ull;            // lobby_poster_array_4k_emi

class TexturePayloads : public ResourceTransforms {};

TEST_F(TexturePayloads, ABufferOneEntryIsWrittenOverTheGamesPayloadAndBufferZeroIsLeftAlone) {
  static const std::uint8_t payload[] = {'D', 'D', 'S', ' ', 1, 2};
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, payload, sizeof(payload), "poster");
  std::uint8_t descriptor[4] = {7, 7, 7, 7};
  GameFinishesBuffer(kTextureType, kPosterName, 0, descriptor, sizeof(descriptor));
  EXPECT_EQ(g_seenBuffer, descriptor) << "buffer 0 was replaced by an entry that names buffer 1";
  EXPECT_EQ(g_seenSize, sizeof(descriptor));
  EXPECT_EQ(descriptor[0], 7);
  EXPECT_FALSE(TestLogContains("override applied"));

  std::uint8_t gameBuffer[8] = {0, 0, 0, 0, 0, 0, 0xAA, 0xAA};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_seenBuffer, gameBuffer) << "the payload is written in place: the game keeps its own buffer";
  EXPECT_EQ(g_seenSize, sizeof(payload));
  EXPECT_EQ(std::memcmp(gameBuffer, payload, sizeof(payload)), 0);
  EXPECT_TRUE(g_heap.blocks.empty()) << "no payload-sized allocation in the game's heap";
  EXPECT_TRUE(TestLogContains("override applied label=poster buffer=1 "));
  EXPECT_TRUE(TestLogContains("orig_bytes=8 new_bytes=6 copy=in_place"));
}

TEST_F(TexturePayloads, APayloadLargerThanTheGamesBufferIsNotServedAndTheWarningSaysWhy) {
  static const std::uint8_t payload[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, payload, sizeof(payload), "poster");
  std::uint8_t small[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  GameFinishesBuffer(kTextureType, kPosterName, 1, small, sizeof(small));
  EXPECT_EQ(g_seenBuffer, small);
  EXPECT_EQ(g_seenSize, sizeof(small));
  EXPECT_EQ(small[0], 0xAA) << "nothing is written into a buffer the payload does not fit";
  EXPECT_TRUE(TestLogContains("payload not replaced label=poster buffer=1 "));
  EXPECT_TRUE(TestLogContains("game_bytes=8 new_bytes=10 reason=larger_than_the_game_buffer"));
  EXPECT_FALSE(TestLogContains("override applied"));

  std::uint8_t large[16] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, large, sizeof(large));
  EXPECT_EQ(g_seenSize, sizeof(payload)) << "a payload not served is offered again on the next load";
  EXPECT_EQ(std::memcmp(large, payload, sizeof(payload)), 0);
}

TEST_F(TexturePayloads, AnEntryKeyedByTheBufferZeroOnDiskTypeReplacesTheDescriptorOnly) {
  static const std::uint8_t descriptor[] = {1, 2, 3, 4, 5};
  RegisterResourceOverride(kTextureDescriptorKey, kPosterName, descriptor, sizeof(descriptor), "poster_desc");
  std::uint8_t gameBuffer[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_seenSize, sizeof(gameBuffer));
  static const std::uint8_t stock[] = {9, 9, 9};
  GameFinishesBuffer(kTextureType, kPosterName, 0, stock, sizeof(stock));
  ExpectGameCopy(descriptor, sizeof(descriptor));
  EXPECT_TRUE(TestLogContains("override applied label=poster_desc buffer=0 "));
  EXPECT_TRUE(TestLogContains("copy=game_heap"));
}

// Regression guards (they pass before the change too): an entry keyed as before #1421, by the logical type or by 0,
// still names buffer 0 and never reaches the payload, so an entry written for an older runtime replaces what it did.
TEST_F(TexturePayloads, AnEntryKeyedByTheLogicalTypeNeverReachesThePayload) {
  static const std::uint8_t bytes[] = {1, 2, 3};
  RegisterResourceOverride(kTextureType, kPosterName, bytes, sizeof(bytes), "logical");
  std::uint8_t gameBuffer[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_seenBuffer, gameBuffer);
  EXPECT_EQ(g_seenSize, sizeof(gameBuffer));
  EXPECT_EQ(gameBuffer[0], 0);
  static const std::uint8_t stock[] = {9, 9, 9, 9};
  GameFinishesBuffer(kTextureType, kPosterName, 0, stock, sizeof(stock));
  ExpectGameCopy(bytes, sizeof(bytes));
}

TEST_F(TexturePayloads, AnEntryKeyedByZeroNeverReachesThePayload) {
  static const std::uint8_t bytes[] = {4, 5, 6};
  RegisterResourceOverride(0, kPosterName, bytes, sizeof(bytes), "any_type");
  std::uint8_t gameBuffer[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_seenSize, sizeof(gameBuffer));
  EXPECT_EQ(gameBuffer[0], 0);
  static const std::uint8_t stock[] = {9, 9, 9, 9};
  GameFinishesBuffer(kTextureType, kPosterName, 0, stock, sizeof(stock));
  ExpectGameCopy(bytes, sizeof(bytes));
}

TEST_F(TexturePayloads, NoEntryNamesABufferPastTheSecond) {
  static const std::uint8_t bytes[] = {1, 2, 3};
  RegisterResourceOverride(0, kPosterName, bytes, sizeof(bytes), "any_type");
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, bytes, sizeof(bytes), "poster");
  std::uint8_t gameBuffer[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 2, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_seenBuffer, gameBuffer);
  EXPECT_EQ(g_seenSize, sizeof(gameBuffer));
  EXPECT_EQ(gameBuffer[0], 0);
  EXPECT_FALSE(TestLogContains("override applied"));
}

TEST_F(TexturePayloads, AnEmptyPayloadIsNotWrittenAndTheWarningSaysWhy) {
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, nullptr, 0, "empty");
  std::uint8_t gameBuffer[8] = {5, 5, 5, 5, 5, 5, 5, 5};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_seenSize, sizeof(gameBuffer)) << "the game's payload keeps its size";
  EXPECT_TRUE(TestLogContains("payload not replaced label=empty buffer=1 "));
  EXPECT_TRUE(TestLogContains("reason=no_bytes"));
}

TEST_F(TexturePayloads, ABlockWithNoGameBufferIsNotWrittenAndTheWarningSaysWhy) {
  static const std::uint8_t payload[] = {1, 2, 3};
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, payload, sizeof(payload), "poster");
  GameFinishesBuffer(kTextureType, kPosterName, 1, nullptr, 8);
  EXPECT_EQ(g_seenBuffer, nullptr);
  EXPECT_EQ(g_seenSize, 8u);
  EXPECT_TRUE(TestLogContains("reason=no_game_buffer"));
}

TEST_F(TexturePayloads, ATransformIsNeverOfferedThePayload) {
  RegisterResourceTransform(kTexturePayloadKey, kPosterName, &ProbeTransform, nullptr, "probe");
  std::uint8_t gameBuffer[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_probe.calls, 0);
  EXPECT_EQ(g_seenSize, sizeof(gameBuffer));
}

TEST_F(TexturePayloads, ACachedPayloadIsWrittenOverBufferOne) {
  const std::vector<std::uint8_t> image = Image(3000, 5);
  Cache(image, kTexturePayloadKey, kPosterName, "cache_poster");
  ASSERT_EQ(RegisterCachedResourceOverrides(dir_.c_str()), 1u);
  std::vector<std::uint8_t> gameBuffer(4096, 0xAA);
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer.data(), gameBuffer.size());
  EXPECT_EQ(g_seenBuffer, gameBuffer.data());
  EXPECT_EQ(g_seenSize, image.size());
  EXPECT_EQ(std::memcmp(gameBuffer.data(), image.data(), image.size()), 0);
  EXPECT_TRUE(TestLogContains("override applied label=cache_poster buffer=1 "));
}

// The built-in splash has always been registered under Make(CGTextureResource, "Win10GPU") with a payload of
// ui_menu_frontend's buffer-1 size (#1130); matched against the logical type it never applied.
TEST_F(TexturePayloads, TheBuiltInSplashIsWrittenOverTheMenuTexturesPayload) {
  TestHook_RegisterInitialOverrides(dir_.c_str());
  ASSERT_TRUE(TestLogContains("Registered embedded override: splash_texture"));
  std::uint8_t descriptor[256] = {};
  GameFinishesBuffer(kTextureType, kSplashName, 0, descriptor, sizeof(descriptor));
  EXPECT_EQ(g_seenBuffer, descriptor);
  EXPECT_EQ(g_seenSize, sizeof(descriptor));
  std::vector<std::uint8_t> gameBuffer(1050548, 0);
  GameFinishesBuffer(kTextureType, kSplashName, 1, gameBuffer.data(), gameBuffer.size());
  EXPECT_EQ(g_seenBuffer, gameBuffer.data());
  EXPECT_EQ(g_seenSize, 1050548u);
  EXPECT_EQ(std::memcmp(gameBuffer.data(), "DDS ", 4), 0);
  EXPECT_TRUE(TestLogContains("override applied label=splash_texture buffer=1 "));
  EXPECT_TRUE(TestLogContains("orig_bytes=1050548 new_bytes=1050548 copy=in_place"));
}

// A dedicated server has no menu: with g_isServer set, the built-in splash is not registered and the game's buffer is kept.
TEST_F(TexturePayloads, AServerRegistersNoBuiltInSplashAndKeepsTheGamesMenuPayload) {
  g_isServer = TRUE;
  TestHook_RegisterInitialOverrides(dir_.c_str());
  EXPECT_TRUE(TestLogContains("built-in splash skipped on a dedicated server (no menu is drawn)"));
  EXPECT_FALSE(TestLogContains("Registered embedded override: splash_texture"));
  std::vector<std::uint8_t> gameBuffer(1050548, 0x5A);
  GameFinishesBuffer(kTextureType, kSplashName, 1, gameBuffer.data(), gameBuffer.size());
  EXPECT_EQ(g_seenBuffer, gameBuffer.data());
  EXPECT_EQ(g_seenSize, 1050548u);
  EXPECT_FALSE(TestLogContains("override applied label=splash_texture"));
}

// The client counterpart: with g_isServer clear, the same call registers the splash and writes it over buffer one.
TEST_F(TexturePayloads, AClientRegistersTheBuiltInSplashAndWritesItOverBufferOne) {
  g_isServer = FALSE;
  TestHook_RegisterInitialOverrides(dir_.c_str());
  EXPECT_TRUE(TestLogContains("Registered embedded override: splash_texture"));
  EXPECT_FALSE(TestLogContains("built-in splash skipped"));
  std::vector<std::uint8_t> gameBuffer(1050548, 0);
  GameFinishesBuffer(kTextureType, kSplashName, 1, gameBuffer.data(), gameBuffer.size());
  EXPECT_TRUE(TestLogContains("override applied label=splash_texture buffer=1 "));
}

// A cached entry covers the built-in splash only when it names the same buffer: one keyed by 0 names buffer 0.
TEST_F(TexturePayloads, ACachedEntryForAnotherBufferDoesNotSuppressTheBuiltInSplash) {
  Cache(Image(90, 3), 0, kSplashName, "any_type");
  TestHook_RegisterInitialOverrides(dir_.c_str());
  EXPECT_TRUE(TestLogContains("Registered embedded override: splash_texture"));
  EXPECT_FALSE(TestLogContains("not registered reason"));
}

// A texture's descriptor and payload are two entries (resource_buffer_key.h): a Warning says when they end up unpaired.
TEST_F(TexturePayloads, ARefusedPayloadSaysItsTextureHasADescriptorEntry) {
  static const std::uint8_t descriptor[] = {1, 2, 3, 4};
  static const std::uint8_t payload[12] = {};
  RegisterResourceOverride(kTextureDescriptorKey, kPosterName, descriptor, sizeof(descriptor), "poster_desc");
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, payload, sizeof(payload), "poster");
  static const std::uint8_t stock[] = {9, 9, 9};
  GameFinishesBuffer(kTextureType, kPosterName, 0, stock, sizeof(stock));
  ExpectGameCopy(descriptor, sizeof(descriptor));
  std::uint8_t small[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, small, sizeof(small));
  EXPECT_EQ(g_seenSize, sizeof(small));
  EXPECT_TRUE(TestLogContains("reason=larger_than_the_game_buffer descriptor_entry=yes "));
  EXPECT_TRUE(TestLogContains("this texture has a descriptor entry, which is served on its own"));
}

TEST_F(TexturePayloads, ARefusedPayloadWithNoDescriptorEntrySaysSo) {
  static const std::uint8_t payload[12] = {};
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, payload, sizeof(payload), "poster");
  RegisterResourceOverride(kTextureDescriptorKey, kPosterName + 1, payload, 4, "other_texture_desc");
  std::uint8_t small[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, small, sizeof(small));
  EXPECT_TRUE(TestLogContains("reason=larger_than_the_game_buffer descriptor_entry=no "));
  EXPECT_FALSE(TestLogContains("served on its own"));
}

TEST_F(TexturePayloads, ASmallerPayloadWithNoDescriptorEntryIsWrittenAndTheWarningSaysSo) {
  static const std::uint8_t payload[6] = {1, 2, 3, 4, 5, 6};
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, payload, sizeof(payload), "poster");
  RegisterResourceTransform(kTextureType, kPosterName, &ProbeTransform, nullptr, "probe");
  std::uint8_t gameBuffer[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_seenSize, sizeof(payload));
  EXPECT_TRUE(TestLogContains("payload smaller than the game's label=poster buffer=1 "));
  EXPECT_TRUE(TestLogContains("game_bytes=8 new_bytes=6 descriptor_entry=no "));
}

TEST_F(TexturePayloads, ASmallerPayloadWithItsDescriptorEntryIsWrittenWithNoWarning) {
  static const std::uint8_t descriptor[] = {1, 2, 3, 4};
  static const std::uint8_t payload[6] = {1, 2, 3, 4, 5, 6};
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, payload, sizeof(payload), "poster");
  RegisterResourceOverride(kTextureType, kPosterName, descriptor, sizeof(descriptor), "poster_desc");
  std::uint8_t gameBuffer[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(g_seenSize, sizeof(payload));
  EXPECT_FALSE(TestLogContains("payload smaller than"));
}

TEST_F(TexturePayloads, APayloadOfTheGamesSizeWithNoDescriptorEntryIsWrittenWithNoWarning) {
  static const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  RegisterResourceOverride(kTexturePayloadKey, kPosterName, payload, sizeof(payload), "poster");
  std::uint8_t gameBuffer[8] = {};
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer, sizeof(gameBuffer));
  EXPECT_EQ(std::memcmp(gameBuffer, payload, sizeof(payload)), 0);
  EXPECT_FALSE(TestLogContains("payload smaller than"));
}

// _overrides/ (#1421 Work step 3): a file named for the buffer-1 type replaces the payload, and a file named the old
// way (by name only) stays buffer 0. The scan in InstallResourceOverride reads each name with ParseOverrideFileName and
// registers the file with RegisterResourceOverrideFromFile, as here.
TEST_F(TexturePayloads, AnOverridesFileNamedForBufferOneReplacesThePayloadAndAnOldFormFileStaysBufferZero) {
  std::uint64_t payloadType = 1, payloadName = 0, oldType = 1, oldName = 0, unused = 0;
  ASSERT_TRUE(ParseOverrideFileName("0xbeac1969cb7b8861.0xa240a2bc051b2f23", &payloadType, &payloadName));
  EXPECT_EQ(payloadType, kTexturePayloadKey);
  EXPECT_EQ(payloadName, kPosterName);
  ASSERT_TRUE(ParseOverrideFileName("0Xa240a2bc051b2f23", &oldType, &oldName));
  EXPECT_EQ(oldType, 0u);
  EXPECT_EQ(oldName, kPosterName);
  std::uint64_t dottedType = 1, dottedName = 0;
  ASSERT_TRUE(ParseOverrideFileName("0xa240a2bc051b2f23.dds", &dottedType, &dottedName));
  EXPECT_EQ(dottedType, 0u);
  EXPECT_EQ(dottedName, kPosterName);
  for (const char* oldForm : {"0xa240a2bc051b2f23.0", "0xa240a2bc051b2f23.xx5"}) {
    std::uint64_t t = 1, n = 0;
    ASSERT_TRUE(ParseOverrideFileName(oldForm, &t, &n)) << oldForm;
    EXPECT_EQ(t, 0u) << oldForm;
    EXPECT_EQ(n, kPosterName) << oldForm;
  }
  EXPECT_FALSE(ParseOverrideFileName("poster.dds", &unused, &unused));
  EXPECT_FALSE(ParseOverrideFileName("0x0", &unused, &unused));
  EXPECT_FALSE(ParseOverrideFileName("0xbeac1969cb7b8861.0x0", &unused, &unused));

  char tmp[MAX_PATH];
  ASSERT_NE(GetTempPathA(MAX_PATH, tmp), 0u);
  const std::string stem = std::string(tmp) + "nevr-overrides-" + std::to_string(GetCurrentProcessId());
  const std::string payloadPath = stem + "-0xbeac1969cb7b8861.0xa240a2bc051b2f23";
  const std::string oldPath = stem + "-0xa240a2bc051b2f23";
  const std::vector<std::uint8_t> payload = Image(8, 3);
  const std::vector<std::uint8_t> descriptor = Image(5, 11);
  for (const auto& [path, bytes] : {std::pair{payloadPath, payload}, std::pair{oldPath, descriptor}}) {
    FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), f), bytes.size());
    std::fclose(f);
  }
  RegisterResourceOverrideFromFile(payloadType, payloadName, payloadPath.c_str(), "file_payload");
  RegisterResourceOverrideFromFile(oldType, oldName, oldPath.c_str(), "file_old_form");
  std::remove(payloadPath.c_str());
  std::remove(oldPath.c_str());

  std::vector<std::uint8_t> gameBuffer(8, 0);
  GameFinishesBuffer(kTextureType, kPosterName, 1, gameBuffer.data(), gameBuffer.size());
  EXPECT_EQ(gameBuffer, payload);
  EXPECT_TRUE(TestLogContains("override applied label=file_payload buffer=1 "));
  static const std::uint8_t stock[] = {9, 9, 9};
  GameFinishesBuffer(kTextureType, kPosterName, 0, stock, sizeof(stock));
  ExpectGameCopy(descriptor.data(), descriptor.size());
  EXPECT_TRUE(TestLogContains("override applied label=file_old_form buffer=0 "));
}

TEST_F(TexturePayloads, TheCacheCapHoldsTheShippedPosterSet) {
  Cache(Image(100, 1), kTexturePayloadKey, kPosterName, "cache_poster");
  RegisterCachedResourceOverrides(dir_.c_str());
  EXPECT_TRUE(TestLogContains(" cap=536870912 "));
}


// ── #1420: the netgun bullet effects, through the seam ───────────────────────────────────────────────────────────
// The production transforms (netgun_bullets.cpp) registered by Install, offered the game's buffer by the real
// callback, and the bytes the engine is left holding. The resources are the synthetic ones of
// netgun_bullets_fixture.h; a test pins their digests in place of the stock ones (TestHook_SetPins).

struct NetgunRegistration {
  std::uint64_t type;
  std::uint64_t name;
  ResourceTransformFn transform;
  void* user;
  std::string label;
};
std::vector<NetgunRegistration> g_netgunRegistrations;

bool CaptureNetgunRegistration(std::uint64_t type, std::uint64_t name, ResourceTransformFn transform, void* user,
                               const char* label) {
  g_netgunRegistrations.push_back({type, name, transform, user, label});
  return true;
}

constexpr std::uint64_t kNetgunParticleType = 0x8ee04a8fa8b1cdb0ULL;  // CSymbol64("CParticleEffectCR")
constexpr std::uint64_t kNetgunBulletType = 0x16bafee3754402a4ULL;    // CSymbol64("CR15NetBulletCR")
constexpr std::uint64_t kCombustion = 0x42670f2bed45703cULL;
constexpr std::uint64_t kDyson = 0x43e2da7914642604ULL;
constexpr std::uint64_t kGauss = 0x43e2da7a0c623a19ULL;

class NetgunBulletTransforms : public ResourceTransforms {
 protected:
  void SetUp() override {
    ResourceTransforms::SetUp();
    nevr_netgun_bullets::TestHook_Reset();
    g_netgunRegistrations.clear();
    // Every expected line below names a digest computed the way the transform computes it. A prefix whose SHA-256
    // provider fails makes both empty and the lines match anyway, so the run stops here instead.
    ASSERT_TRUE(nevr_asset_cache::IsSha256Hex(nevr_asset_cache::Sha256Hex(particles_.data(), particles_.size())));
    ASSERT_TRUE(nevr_asset_cache::IsSha256Hex(nevr_asset_cache::Sha256Hex(bullets_.data(), bullets_.size())));
  }
  void TearDown() override {
    nevr_netgun_bullets::TestHook_Reset();
    ResourceTransforms::TearDown();
  }
  // The fixtures' digests, each first in a list of two, as the four-level digest is first in the stock list.
  void PinFixtures() {
    nevr_netgun_bullets::TestHook_SetPins({nevr_asset_cache::Sha256Hex(particles_.data(), particles_.size()), kDecoy},
                                          {nevr_asset_cache::Sha256Hex(bullets_.data(), bullets_.size()), kDecoy});
  }
  static constexpr char kDecoy[] = "0000000000000000000000000000000000000000000000000000000000000000";
  const std::vector<std::uint8_t> particles_ = netgun_fixture::Particles(netgun_fixture::StockRows());
  const std::vector<std::uint8_t> bullets_ = netgun_fixture::Bullets(netgun_fixture::StockEntries());
};

TEST_F(NetgunBulletTransforms, InstallRegistersBothResourcesOfEachOfTheFiveLevels) {
  nevr_netgun_bullets::Install(&CaptureNetgunRegistration);
  const std::pair<const char*, std::uint64_t> levels[] = {
      {"mpl_combat_combustion", kCombustion},
      {"mpl_combat_dyson", kDyson},
      {"mpl_combat_fission", 0xdf5ca7b7dfa383d4ULL},
      {"mpl_combat_gauss", kGauss},
      {"mpl_lobby_b_combat", 0xcb9977f7fc2b4526ULL},
  };
  ASSERT_EQ(g_netgunRegistrations.size(), 10u);
  for (size_t i = 0; i < 5; ++i) {
    const NetgunRegistration& p = g_netgunRegistrations[2 * i];
    const NetgunRegistration& b = g_netgunRegistrations[2 * i + 1];
    EXPECT_EQ(p.type, kNetgunParticleType) << levels[i].first;
    EXPECT_EQ(b.type, kNetgunBulletType) << levels[i].first;
    EXPECT_EQ(p.name, levels[i].second) << levels[i].first;
    EXPECT_EQ(b.name, levels[i].second) << levels[i].first;
    ASSERT_NE(p.user, nullptr);
    EXPECT_STREQ(static_cast<const nevr_netgun_bullets::Level*>(p.user)->name, levels[i].first);
    EXPECT_EQ(p.user, b.user);
    EXPECT_NE(p.transform, b.transform);
    EXPECT_EQ(p.label, std::string("netgun bullet effects ") + levels[i].first + " CParticleEffectCR");
    EXPECT_EQ(b.label, std::string("netgun bullet effects ") + levels[i].first + " CR15NetBulletCR");
  }
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] netgun bullet effects requested levels=5 transforms=10 "
      "pins=7130808e89f3aa9400cb09fb7a30ca85c9a5a0be363557a647974ebd00283b8c,"
      "9f03344fa5e5ea183ffed2b42186a1fa6b7e214953dcbfa1c0308389e9f05750,"
      "611f0ab4d6ada7f275ab1aaf974a08209d3c6e78e1a81b40c88fa11d9a215e6b (once a combat level loads"));
}

TEST_F(NetgunBulletTransforms, APinnedResourceOfALevelIsWhatTheGameReadsAfterTheLoad) {
  nevr_netgun_bullets::Install();
  PinFixtures();
  GameFinishesBuffer(kNetgunParticleType, kCombustion, 0, particles_.data(), particles_.size());
  const std::vector<std::uint8_t> particles = netgun_fixture::PatchedParticles();
  ExpectGameCopy(particles.data(), particles.size());
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Info,
      "netgun bullet effects added level=mpl_combat_combustion resource=CParticleEffectCR entries=11->20"));
  GameFinishesBuffer(kNetgunBulletType, kGauss, 0, bullets_.data(), bullets_.size());
  const std::vector<std::uint8_t> bullets = netgun_fixture::PatchedBullets();
  ExpectGameCopy(bullets.data(), bullets.size());
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "netgun bullet effects added level=mpl_combat_gauss resource=CR15NetBulletCR "
                                     "entries=9->9"));
  // The line names the sizes and digests of what the game delivered and of what it reads now.
  const auto sha = [](const std::vector<std::uint8_t>& b) { return nevr_asset_cache::Sha256Hex(b.data(), b.size()); };
  EXPECT_TRUE(TestLogContains("entries=11->20 orig_bytes=" + std::to_string(particles_.size()) + " new_bytes=" +
                              std::to_string(particles.size()) + " stock_sha256=" + sha(particles_) +
                              " result_sha256=" + sha(particles) + " ("));
  EXPECT_TRUE(TestLogContains("entries=9->9 orig_bytes=" + std::to_string(bullets_.size()) + " new_bytes=" +
                              std::to_string(bullets.size()) + " stock_sha256=" + sha(bullets_) +
                              " result_sha256=" + sha(bullets) + " ("));
}

TEST_F(NetgunBulletTransforms, ALaterLoadOfTheSameBytesIsServedFromTheFirstRewrite) {
  nevr_netgun_bullets::Install();
  PinFixtures();
  GameFinishesBuffer(kNetgunParticleType, kCombustion, 0, particles_.data(), particles_.size());
  ASSERT_TRUE(TestLogContains("netgun bullet effects added level=mpl_combat_combustion"));
  ClearTestLogs();
  GameFinishesBuffer(kNetgunParticleType, kDyson, 0, particles_.data(), particles_.size());
  const std::vector<std::uint8_t> want = netgun_fixture::PatchedParticles();
  ExpectGameCopy(want.data(), want.size());
  EXPECT_TRUE(TestLogContains("transform applied label=netgun bullet effects mpl_combat_dyson CParticleEffectCR"));
  EXPECT_FALSE(TestLogContains("netgun bullet effects added"));  // the first rewrite, not a second one
}

TEST_F(NetgunBulletTransforms, BytesThatAreNotTheStockResourceAreLeftAloneAndSaySo) {
  nevr_netgun_bullets::Install();  // the stock digests: the fixture is none of them
  GameFinishesBuffer(kNetgunParticleType, kCombustion, 0, particles_.data(), particles_.size());
  EXPECT_EQ(g_seenBuffer, particles_.data());
  EXPECT_EQ(g_seenSize, particles_.size());
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "netgun bullet effects left off level=mpl_combat_combustion "
                                     "resource=CParticleEffectCR reason=not_pinned"));
  // The line names the size and digest of what the game delivered, so a run on other data says what it got.
  EXPECT_TRUE(TestLogContains("reason=not_pinned bytes=" + std::to_string(particles_.size()) + " sha256=" +
                              nevr_asset_cache::Sha256Hex(particles_.data(), particles_.size()) + " pins="));
  // The digests it compared against are the stock ones of this resource.
  EXPECT_TRUE(TestLogContains("pins=7130808e89f3aa9400cb09fb7a30ca85c9a5a0be363557a647974ebd00283b8c,"
                              "9f03344fa5e5ea183ffed2b42186a1fa6b7e214953dcbfa1c0308389e9f05750 "));
  GameFinishesBuffer(kNetgunBulletType, kCombustion, 0, bullets_.data(), bullets_.size());
  EXPECT_EQ(g_seenBuffer, bullets_.data());
  EXPECT_TRUE(TestLogContains("resource=CR15NetBulletCR reason=not_pinned bytes=" + std::to_string(bullets_.size()) +
                              " sha256=" + nevr_asset_cache::Sha256Hex(bullets_.data(), bullets_.size()) + " pins="));
  EXPECT_TRUE(TestLogContains("pins=611f0ab4d6ada7f275ab1aaf974a08209d3c6e78e1a81b40c88fa11d9a215e6b "));
  // Left alone means no rewrite was made or kept.
  EXPECT_FALSE(TestLogContains("netgun bullet effects added"));
}

std::string FailedDigest(const void*, size_t) { return std::string(); }  // what Sha256Hex returns when CryptoAPI fails

TEST_F(NetgunBulletTransforms, BytesWhoseDigestCannotBeComputedAreLeftAloneAndSayWhy) {
  nevr_netgun_bullets::Install();
  PinFixtures();  // the bytes are stock: only the digest of them fails
  nevr_netgun_bullets::TestHook_SetDigest(&FailedDigest);
  GameFinishesBuffer(kNetgunParticleType, kCombustion, 0, particles_.data(), particles_.size());
  EXPECT_EQ(g_seenBuffer, particles_.data());
  EXPECT_EQ(g_seenSize, particles_.size());
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "netgun bullet effects left off level=mpl_combat_combustion "
                                     "resource=CParticleEffectCR reason=digest_failed bytes=" +
                                         std::to_string(particles_.size()) + " ("));
  // The failure is this machine's, not the data's: it is not reported as bytes that match no pin.
  EXPECT_FALSE(TestLogContains("reason=not_pinned"));
  EXPECT_FALSE(TestLogContains("netgun bullet effects added"));
  // After a reset (which puts back the real digest), the same stock bytes are rewritten: nothing of the failed load
  // was kept.
  nevr_netgun_bullets::TestHook_Reset();
  PinFixtures();
  GameFinishesBuffer(kNetgunParticleType, kCombustion, 0, particles_.data(), particles_.size());
  EXPECT_NE(g_seenBuffer, particles_.data());
  EXPECT_TRUE(TestLogContains("netgun bullet effects added level=mpl_combat_combustion"));
}

TEST_F(NetgunBulletTransforms, ATransformGivenNoBufferServesNothingAndSaysSo) {
  nevr_netgun_bullets::Install(&CaptureNetgunRegistration);
  ASSERT_EQ(g_netgunRegistrations.size(), 10u);
  const void* out = nullptr;
  std::uint64_t outSize = 0;
  const NetgunRegistration& r = g_netgunRegistrations[3];  // mpl_combat_dyson, CR15NetBulletCR
  EXPECT_FALSE(r.transform(nullptr, 8, &out, &outSize, r.user));
  EXPECT_TRUE(TestLogContainsAtLevel(
      EchoVR::LogLevel::Warning,
      "netgun bullet effects left off level=mpl_combat_dyson resource=CR15NetBulletCR reason=no_buffer bytes=8 ("));
  ClearTestLogs();
  EXPECT_FALSE(r.transform(bullets_.data(), 0, &out, &outSize, r.user));
  EXPECT_TRUE(TestLogContains(
      "netgun bullet effects left off level=mpl_combat_dyson resource=CR15NetBulletCR reason=no_buffer bytes=0"));
  // An empty buffer is not compared with the pins: the no-buffer line is the only decision.
  EXPECT_FALSE(TestLogContains("reason=not_pinned"));
  EXPECT_EQ(out, nullptr);
}

TEST_F(NetgunBulletTransforms, APinnedResourceWithoutTheStockLayoutIsLeftAloneAndSaysWhy) {
  nevr_netgun_bullets::Install();
  const std::vector<std::uint8_t> patched = netgun_fixture::PatchedBullets();
  nevr_netgun_bullets::TestHook_SetPins({}, {nevr_asset_cache::Sha256Hex(patched.data(), patched.size())});
  GameFinishesBuffer(kNetgunBulletType, kDyson, 0, patched.data(), patched.size());
  EXPECT_EQ(g_seenBuffer, patched.data());
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "netgun bullet effects left off level=mpl_combat_dyson resource=CR15NetBulletCR "
                                     "reason=already_present sha256=" +
                                         nevr_asset_cache::Sha256Hex(patched.data(), patched.size()) + " ("));
  EXPECT_FALSE(TestLogContains("netgun bullet effects added"));
}

TEST_F(NetgunBulletTransforms, TheRewriteOfOneResourceIsNeverServedForTheOther) {
  // The same bytes pinned for both resources: the particle rewrite is kept, and the bullet transform still reads the
  // bytes as a CR15NetBulletCR (whose layout they do not have) instead of serving the particle rewrite.
  nevr_netgun_bullets::Install();
  const std::string sha = nevr_asset_cache::Sha256Hex(particles_.data(), particles_.size());
  nevr_netgun_bullets::TestHook_SetPins({sha}, {sha});
  GameFinishesBuffer(kNetgunParticleType, kDyson, 0, particles_.data(), particles_.size());
  EXPECT_NE(g_seenBuffer, particles_.data());
  GameFinishesBuffer(kNetgunBulletType, kDyson, 0, particles_.data(), particles_.size());
  EXPECT_EQ(g_seenBuffer, particles_.data());
  EXPECT_TRUE(TestLogContains("netgun bullet effects left off level=mpl_combat_dyson resource=CR15NetBulletCR "
                              "reason=bad_header"));
}

TEST_F(NetgunBulletTransforms, AnotherLevelOrAnotherClassIsNotAnswered) {
  nevr_netgun_bullets::Install();
  PinFixtures();
  GameFinishesBuffer(kNetgunParticleType, 0x1234, 0, particles_.data(), particles_.size());
  EXPECT_EQ(g_seenBuffer, particles_.data());
  GameFinishesBuffer(kNetgunParticleType + 1, kCombustion, 0, particles_.data(), particles_.size());
  EXPECT_EQ(g_seenBuffer, particles_.data());
  EXPECT_FALSE(TestLogContains("netgun bullet effects added"));
}

}  // namespace
