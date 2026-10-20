// #703: the content-addressed asset cache (patch/asset_cache.cpp), driven for real against a temp directory.
// Built into test_asset_cache; run under Wine by `just test-auth-unit`.

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "runtime/patch/asset_cache.h"

namespace {

namespace cache = nevr_asset_cache;

std::vector<std::uint8_t> Bytes(const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

std::vector<std::uint8_t> Filler(std::size_t n, std::uint8_t seed) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 31);
  return v;
}

// A fresh directory per test; UTF-16 on purpose ("é", a CJK character): the user name in %LOCALAPPDATA% may not be ASCII.
class AssetCache : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    dir_ = std::wstring(tmp) + L"nevr-asset-cache-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::wstring(test.begin(), test.end()) + L"-\u00e9\u4e2d";
    Wipe();
  }
  void TearDown() override { Wipe(); }

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

  std::string StoreOk(const std::vector<std::uint8_t>& data) {
    const cache::StoreResult r = cache::Store(dir_, data.data(), data.size());
    EXPECT_NE(r.status, cache::StoreStatus::Failed) << r.failedStep << " error " << r.error;
    return r.sha256;
  }

  static void SetAge(const std::wstring& path, unsigned seconds) {
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(f, INVALID_HANDLE_VALUE);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t;
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart -= static_cast<std::uint64_t>(seconds) * 10000000ull;
    FILETIME then;
    then.dwLowDateTime = t.LowPart;
    then.dwHighDateTime = t.HighPart;
    ASSERT_NE(SetFileTime(f, nullptr, nullptr, &then), 0);
    CloseHandle(f);
  }

  static void WriteRaw(const std::wstring& path, const std::vector<std::uint8_t>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(f, INVALID_HANDLE_VALUE);
    DWORD put = 0;
    ASSERT_NE(WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &put, nullptr), 0);
    CloseHandle(f);
  }

  bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

  std::wstring dir_;
};

TEST_F(AssetCache, Sha256MatchesTheKnownVector) {
  EXPECT_EQ(cache::Sha256Hex("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(cache::Sha256Hex("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(AssetCache, AKeyIsExactly64LowercaseHexCharacters) {
  const std::string good(64, 'a');
  EXPECT_TRUE(cache::IsSha256Hex(good));
  EXPECT_FALSE(cache::IsSha256Hex(std::string(63, 'a')));
  EXPECT_FALSE(cache::IsSha256Hex(std::string(65, 'a')));
  EXPECT_FALSE(cache::IsSha256Hex(std::string(64, 'A')));
  EXPECT_FALSE(cache::IsSha256Hex(std::string(63, 'a') + "g"));
  EXPECT_FALSE(cache::IsSha256Hex("..\\" + std::string(61, 'a')));
}

TEST_F(AssetCache, StoredBytesComeBackUnderTheirOwnHash) {
  const std::vector<std::uint8_t> data = Filler(4096, 7);
  const cache::StoreResult stored = cache::Store(dir_, data.data(), data.size());
  ASSERT_EQ(stored.status, cache::StoreStatus::Stored) << stored.failedStep << " " << stored.error;
  EXPECT_EQ(stored.sha256, cache::Sha256Hex(data.data(), data.size()));
  EXPECT_TRUE(Exists(cache::BlobPath(dir_, stored.sha256)));
  const cache::LoadResult loaded = cache::Load(dir_, stored.sha256);
  ASSERT_EQ(loaded.status, cache::LoadStatus::Hit);
  EXPECT_EQ(loaded.bytes, data);
  EXPECT_EQ(loaded.actualSha256, stored.sha256);
}

TEST_F(AssetCache, AGoodBlobIsNotWrittenAgain) {
  const std::vector<std::uint8_t> data = Filler(100, 1);
  ASSERT_EQ(cache::Store(dir_, data.data(), data.size()).status, cache::StoreStatus::Stored);
  EXPECT_EQ(cache::Store(dir_, data.data(), data.size()).status, cache::StoreStatus::AlreadyPresent);
}

TEST_F(AssetCache, ABlobWithOneByteFlippedIsRefusedOnRead) {
  const std::vector<std::uint8_t> data = Filler(2048, 3);
  const std::string sha = StoreOk(data);
  std::vector<std::uint8_t> bad = data;
  bad[1000] ^= 0x01;
  WriteRaw(cache::BlobPath(dir_, sha), bad);
  const cache::LoadResult r = cache::Load(dir_, sha);
  EXPECT_EQ(r.status, cache::LoadStatus::HashMismatch);
  EXPECT_TRUE(r.bytes.empty()) << "refused bytes must not be handed out";
  EXPECT_EQ(r.expectedSha256, sha);
  EXPECT_EQ(r.actualSha256, cache::Sha256Hex(bad.data(), bad.size()));
  EXPECT_NE(r.actualSha256, sha);
}

TEST_F(AssetCache, ATruncatedBlobIsRefusedAndStoreReplacesIt) {
  const std::vector<std::uint8_t> data = Filler(5000, 9);
  const std::string sha = StoreOk(data);
  WriteRaw(cache::BlobPath(dir_, sha), std::vector<std::uint8_t>(data.begin(), data.begin() + 100));
  EXPECT_EQ(cache::Load(dir_, sha).status, cache::LoadStatus::HashMismatch);
  EXPECT_EQ(cache::Store(dir_, data.data(), data.size()).status, cache::StoreStatus::Stored);
  EXPECT_EQ(cache::Load(dir_, sha).status, cache::LoadStatus::Hit);
}

TEST_F(AssetCache, AMissingBlobIsMissingNotAnError) {
  EXPECT_EQ(cache::Load(dir_, std::string(64, 'b')).status, cache::LoadStatus::Missing);
}

TEST_F(AssetCache, AKeyThatIsNotAHashNeverReachesTheFilesystem) {
  for (const char* key : {"", "..\\..\\evil", "../x", "ABCDEF", "a"}) {
    EXPECT_EQ(cache::Load(dir_, key).status, cache::LoadStatus::BadKey) << key;
  }
}

TEST_F(AssetCache, AStoreLeavesNoTempFileAndAnInterruptedOneIsNeverRead) {
  const std::vector<std::uint8_t> data = Filler(1024, 4);
  const std::string sha = cache::Sha256Hex(data.data(), data.size());
  // A killed writer leaves only its temp file: no blob under the trusted name.
  ASSERT_TRUE(CreateDirectoryW(dir_.c_str(), nullptr) != 0);
  WriteRaw(dir_ + L"\\" + std::wstring(sha.begin(), sha.end()) + L".1234.5678.tmp",
           std::vector<std::uint8_t>(data.begin(), data.begin() + 10));
  EXPECT_EQ(cache::Load(dir_, sha).status, cache::LoadStatus::Missing);
  ASSERT_EQ(cache::Store(dir_, data.data(), data.size()).status, cache::StoreStatus::Stored);
  int temps = 0;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((dir_ + L"\\*.tmp").c_str(), &fd);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      ++temps;
    } while (FindNextFileW(find, &fd) != 0);
    FindClose(find);
  }
  EXPECT_EQ(temps, 1) << "only the pre-existing leftover; the store's own temp was renamed away";
}

TEST_F(AssetCache, ABlobOverTheSizeLimitIsRefused) {
  const std::vector<std::uint8_t> data = Filler(16, 1);
  const std::string sha = StoreOk(data);
  EXPECT_EQ(cache::Store(dir_, data.data(), static_cast<std::size_t>(cache::kMaxBlobBytes) + 1).status,
            cache::StoreStatus::Failed);
  EXPECT_EQ(cache::Load(dir_, sha).status, cache::LoadStatus::Hit);
}

TEST_F(AssetCache, TheIndexRoundTripsAndOneKeyHasOneEntry) {
  const std::string a(64, 'a'), b(64, 'b'), c(64, 'c');
  std::string err;
  ASSERT_TRUE(cache::SetIndexEntry(dir_, {0xbeac1969cb7b8861ull, 0x1f72c5e0fc9fc8c0ull, a, "cache_splash"}, &err)) << err;
  ASSERT_TRUE(cache::SetIndexEntry(dir_, {0, 0x42ull, b, "other"}, &err)) << err;
  ASSERT_TRUE(cache::SetIndexEntry(dir_, {0xbeac1969cb7b8861ull, 0x1f72c5e0fc9fc8c0ull, c, "cache_splash"}, &err)) << err;
  const cache::IndexResult r = cache::ReadIndex(dir_);
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.entries.size(), 2u);
  EXPECT_EQ(r.entries[0].typeHash, 0xbeac1969cb7b8861ull);
  EXPECT_EQ(r.entries[0].nameHash, 0x1f72c5e0fc9fc8c0ull);
  EXPECT_EQ(r.entries[0].sha256, c) << "the second write for the same (type, name) replaced the first";
  EXPECT_EQ(r.entries[0].label, "cache_splash");
  EXPECT_EQ(r.entries[1].nameHash, 0x42ull);
  EXPECT_EQ(r.entries[1].sha256, b);
}

TEST_F(AssetCache, NoIndexMeansNothingCachedNotAFailure) {
  const cache::IndexResult r = cache::ReadIndex(dir_);
  EXPECT_FALSE(r.present);
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.entries.empty());
}

TEST_F(AssetCache, AMalformedEntryIsSkippedWithAReasonAndTheRestLoad) {
  ASSERT_TRUE(CreateDirectoryW(dir_.c_str(), nullptr) != 0);
  const std::string good(64, 'd');
  const std::string text =
      "{\"version\":1,\"overrides\":["
      "{\"type\":\"0x1\",\"name\":\"0x2\",\"sha256\":\"" + good + "\",\"label\":\"ok\"},"
      "{\"type\":\"0x1\",\"name\":\"0x2\",\"sha256\":\"../../x\"},"
      "{\"type\":\"nope\",\"name\":\"0x2\",\"sha256\":\"" + good + "\"},"
      "{\"type\":\"0x1\",\"name\":\"0x0\",\"sha256\":\"" + good + "\"},"
      "7]}";
  WriteRaw(dir_ + L"\\index.json", Bytes(text));
  const cache::IndexResult r = cache::ReadIndex(dir_);
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.entries.size(), 1u);
  EXPECT_EQ(r.entries[0].label, "ok");
  EXPECT_EQ(r.skipped.size(), 4u);
}

TEST_F(AssetCache, AnUnreadableIndexIsReportedAndNeverOverwritten) {
  ASSERT_TRUE(CreateDirectoryW(dir_.c_str(), nullptr) != 0);
  WriteRaw(dir_ + L"\\index.json", Bytes("{ this is not json"));
  const cache::IndexResult r = cache::ReadIndex(dir_);
  EXPECT_TRUE(r.present);
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.error.empty());
  std::string err;
  EXPECT_FALSE(cache::SetIndexEntry(dir_, {0, 5, std::string(64, 'e'), "x"}, &err));
  EXPECT_FALSE(err.empty());
  EXPECT_FALSE(cache::ReadIndex(dir_).ok) << "the damaged index is left as found";
}

TEST_F(AssetCache, TheIndexRefusesAnEntryWithoutAHashOrAName) {
  std::string err;
  EXPECT_FALSE(cache::SetIndexEntry(dir_, {1, 2, "short", "x"}, &err));
  EXPECT_FALSE(cache::SetIndexEntry(dir_, {1, 0, std::string(64, 'a'), "x"}, &err));
}

TEST_F(AssetCache, EvictionRemovesUnreferencedBlobsLeastRecentlyUsedFirstUntilTheCapFits) {
  const std::string old1 = StoreOk(Filler(1000, 1)), old2 = StoreOk(Filler(1000, 2)), fresh = StoreOk(Filler(1000, 3));
  SetAge(cache::BlobPath(dir_, old1), 300);
  SetAge(cache::BlobPath(dir_, old2), 200);
  SetAge(cache::BlobPath(dir_, fresh), 10);
  const cache::EvictReport r = cache::Evict(dir_, 1500, {});
  EXPECT_EQ(r.blobBytesBefore, 3000u);
  EXPECT_EQ(r.blobsEvicted, 2u);
  EXPECT_EQ(r.blobBytesAfter, 1000u);
  EXPECT_FALSE(Exists(cache::BlobPath(dir_, old1)));
  EXPECT_FALSE(Exists(cache::BlobPath(dir_, old2)));
  EXPECT_TRUE(Exists(cache::BlobPath(dir_, fresh)));
  EXPECT_FALSE(r.overCapAllReferenced);
}

TEST_F(AssetCache, ABlobTheIndexNamesIsNeverEvictedAndTheReportSaysTheCapIsExceeded) {
  const std::string inUse = StoreOk(Filler(1000, 1)), spare = StoreOk(Filler(1000, 2));
  SetAge(cache::BlobPath(dir_, inUse), 999);  // the oldest of all, but in use
  SetAge(cache::BlobPath(dir_, spare), 5);
  cache::EvictReport r = cache::Evict(dir_, 500, {inUse});
  EXPECT_TRUE(Exists(cache::BlobPath(dir_, inUse)));
  EXPECT_FALSE(Exists(cache::BlobPath(dir_, spare)));
  EXPECT_EQ(r.blobBytesAfter, 1000u);
  EXPECT_TRUE(r.overCapAllReferenced);
}

TEST_F(AssetCache, UnderTheCapNothingIsEvicted) {
  const std::string a = StoreOk(Filler(100, 1));
  const cache::EvictReport r = cache::Evict(dir_, 1000, {});
  EXPECT_EQ(r.blobsEvicted, 0u);
  EXPECT_TRUE(Exists(cache::BlobPath(dir_, a)));
}

TEST_F(AssetCache, AReadRefreshesALastUsedStampSoTheBlobOutlivesAnUnreadOne) {
  const std::string readRecently = StoreOk(Filler(1000, 1)), neverRead = StoreOk(Filler(1000, 2));
  SetAge(cache::BlobPath(dir_, readRecently), 500);
  SetAge(cache::BlobPath(dir_, neverRead), 400);  // younger than the first, until the first is read
  ASSERT_EQ(cache::Load(dir_, readRecently).status, cache::LoadStatus::Hit);
  cache::Evict(dir_, 1000, {});
  EXPECT_TRUE(Exists(cache::BlobPath(dir_, readRecently)));
  EXPECT_FALSE(Exists(cache::BlobPath(dir_, neverRead)));
}

TEST_F(AssetCache, StaleTempFilesAreRemovedAndFreshOnesKept) {
  ASSERT_TRUE(CreateDirectoryW(dir_.c_str(), nullptr) != 0);
  const std::wstring stale = dir_ + L"\\" + std::wstring(64, L'a') + L".1.2.tmp";
  const std::wstring fresh = dir_ + L"\\" + std::wstring(64, L'b') + L".3.4.tmp";
  WriteRaw(stale, Bytes("x"));
  WriteRaw(fresh, Bytes("y"));
  SetAge(stale, 2 * 24 * 60 * 60);
  const cache::EvictReport r = cache::Evict(dir_, cache::kDefaultCapBytes, {});
  EXPECT_EQ(r.staleTempsRemoved, 1u);
  EXPECT_FALSE(Exists(stale));
  EXPECT_TRUE(Exists(fresh)) << "a younger temp may belong to a live writer in another process";
}

TEST_F(AssetCache, FilesThatAreNotBlobsAreNeverCountedOrRemoved) {
  ASSERT_TRUE(CreateDirectoryW(dir_.c_str(), nullptr) != 0);
  WriteRaw(dir_ + L"\\index.json", Bytes("{}"));
  WriteRaw(dir_ + L"\\notes.txt", Filler(5000, 1));
  WriteRaw(dir_ + L"\\" + std::wstring(63, L'a') + L".blob", Filler(5000, 2));  // 63 characters: not a key
  const cache::EvictReport r = cache::Evict(dir_, 0, {});
  EXPECT_EQ(r.blobBytesBefore, 0u);
  EXPECT_TRUE(Exists(dir_ + L"\\index.json"));
  EXPECT_TRUE(Exists(dir_ + L"\\notes.txt"));
  EXPECT_TRUE(Exists(dir_ + L"\\" + std::wstring(63, L'a') + L".blob"));
}

TEST_F(AssetCache, TheDefaultDirectoryIsUnderLocalAppDataNevrEchovrCache) {
  SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\\u00e9\u4e2d\\AppData\\Local");
  EXPECT_EQ(cache::DefaultDir(), L"C:\\Users\\\u00e9\u4e2d\\AppData\\Local\\nevr\\echovr\\cache");
  SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  EXPECT_TRUE(cache::DefaultDir().empty());
}

}  // namespace
