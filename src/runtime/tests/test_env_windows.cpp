// The CDN's PC edge (src/cdn/env_windows.cpp), driven for real on a temp directory. Covers files, blobs, SHA-256, device facts and the
// clock. The network call (Get) is not exercised here.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <gtest/gtest.h>

#include <winsock2.h>  // before windows.h
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "cdn/cdn_env.h"
#include "cdn/env_windows.h"

namespace {

constexpr DWORD kTempPathChars = MAX_PATH;
constexpr std::size_t kSmallFileLimit = 0x100000;
constexpr std::uint64_t kReadOffset = 3;
constexpr std::size_t kReadLength = 4;
constexpr std::uint64_t kTailOffset = 8;
constexpr std::uint64_t kTruncateTo = 4;
constexpr const char* kAbcSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr const char* kEmptySha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* kPayload = "abcdefghij";

std::string Utf8(const std::wstring& wide) {
  const int size = static_cast<int>(wide.size());
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, out.data(), needed, nullptr, nullptr);
  return out;
}

class EnvWindowsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t temp[kTempPathChars] = {};
    const DWORD n = GetTempPathW(kTempPathChars, temp);
    ASSERT_GT(n, 0u);
    ASSERT_LT(n, kTempPathChars);
    const std::string base = Utf8(std::wstring(temp, n));  // ends with a backslash
    dir_ = base + "nevr-env-windows-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    ASSERT_TRUE(env_.MakeDirs(dir_));
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string Path(const char* name) const { return dir_ + "\\" + name; }

  nevr_cdn::WindowsEnv env_;
  std::string dir_;
};

TEST_F(EnvWindowsTest, MakeDirsCreatesNestedAndIsIdempotent) {
  const std::string nested = dir_ + "\\a\\b\\c";
  EXPECT_TRUE(env_.MakeDirs(nested));
  EXPECT_TRUE(env_.MakeDirs(nested));
  EXPECT_FALSE(env_.MakeDirs("relative\\dir"));
  std::string text = "file";
  ASSERT_TRUE(env_.WriteFileAtomically(Path("f"), text));
  EXPECT_FALSE(env_.MakeDirs(Path("f") + "\\x"));
}

TEST_F(EnvWindowsTest, WriteFileAtomicallyRoundTripsAndLeavesNoTemp) {
  ASSERT_TRUE(env_.WriteFileAtomically(Path("obj"), "hello"));
  std::string text;
  ASSERT_TRUE(env_.ReadSmallFile(Path("obj"), &text, kSmallFileLimit));
  EXPECT_EQ(text, "hello");
  ASSERT_TRUE(env_.WriteFileAtomically(Path("obj"), "second"));
  ASSERT_TRUE(env_.ReadSmallFile(Path("obj"), &text, kSmallFileLimit));
  EXPECT_EQ(text, "second");
  EXPECT_FALSE(std::filesystem::exists(Path("obj") + ".tmp"));
}

TEST_F(EnvWindowsTest, ReadSmallFileRefusesOverMaxAndDirectories) {
  const std::string payload = kPayload;
  ASSERT_TRUE(env_.WriteFileAtomically(Path("obj"), payload));
  std::string text;
  EXPECT_FALSE(env_.ReadSmallFile(Path("obj"), &text, payload.size() - 1));
  EXPECT_TRUE(env_.ReadSmallFile(Path("obj"), &text, payload.size()));
  EXPECT_EQ(text, payload);
  EXPECT_FALSE(env_.ReadSmallFile(dir_, &text, kSmallFileLimit));
}

TEST_F(EnvWindowsTest, AppendFileAppendsSizesTruncatesAndSyncs) {
  std::unique_ptr<nevr_cdn::AppendFile> file = env_.OpenAppend(Path("log"));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->Size(), 0u);
  const std::string abcdef = "abcdef";
  EXPECT_TRUE(file->Append(abcdef.data(), abcdef.size()));
  EXPECT_EQ(file->Size(), abcdef.size());
  EXPECT_TRUE(file->Sync());
  EXPECT_TRUE(file->Truncate(kTruncateTo));
  EXPECT_EQ(file->Size(), kTruncateTo);
  EXPECT_FALSE(file->Truncate(abcdef.size() + 1));
  const std::string xy = "XY";
  EXPECT_TRUE(file->Append(xy.data(), xy.size()));
  EXPECT_EQ(file->Size(), kTruncateTo + xy.size());
  file.reset();
  std::string text;
  ASSERT_TRUE(env_.ReadSmallFile(Path("log"), &text, kSmallFileLimit));
  EXPECT_EQ(text, "abcdXY");
}

TEST_F(EnvWindowsTest, BlobReadsAtOffsetAndStopsAtEnd) {
  const std::string payload = kPayload;
  ASSERT_TRUE(env_.WriteFileAtomically(Path("blob"), payload));
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  ASSERT_TRUE(env_.OpenBlob(Path("blob"), &handle, &size));
  EXPECT_EQ(size, payload.size());

  std::string buffer(kReadLength, '\0');
  std::size_t got = 0;
  ASSERT_TRUE(env_.ReadBlob(handle, kReadOffset, buffer.data(), buffer.size(), &got));
  EXPECT_EQ(got, kReadLength);
  EXPECT_EQ(buffer, payload.substr(kReadOffset, kReadLength));

  std::string tail(payload.size(), '\0');
  ASSERT_TRUE(env_.ReadBlob(handle, kTailOffset, tail.data(), tail.size(), &got));
  EXPECT_EQ(got, payload.size() - kTailOffset);
  EXPECT_EQ(tail.substr(0, got), payload.substr(kTailOffset));

  ASSERT_TRUE(env_.ReadBlob(handle, payload.size(), tail.data(), kReadLength, &got));
  EXPECT_EQ(got, 0u);
  env_.CloseBlob(handle);
}

TEST_F(EnvWindowsTest, OpenBlobRefusesDirectories) {
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  EXPECT_FALSE(env_.OpenBlob(dir_, &handle, &size));
}

TEST_F(EnvWindowsTest, HasherComputesSha256AndResetsAfterHex) {
  std::unique_ptr<nevr_cdn::Hasher> hasher = env_.NewHasher();
  const std::string abc = "abc";
  hasher->Update(abc.data(), abc.size());
  EXPECT_EQ(hasher->Hex(), kAbcSha256);
  hasher->Update(abc.data(), abc.size());
  EXPECT_EQ(hasher->Hex(), kAbcSha256);
  EXPECT_EQ(hasher->Hex(), kEmptySha256);
}

TEST_F(EnvWindowsTest, QueryDeviceReportsEveryFact) {
  nevr_cdn::DeviceFacts facts;
  ASSERT_TRUE(env_.QueryDevice(dir_, &facts));
  EXPECT_TRUE(facts.have_free);
  EXPECT_TRUE(facts.have_memory);
  EXPECT_TRUE(facts.have_fds);
  EXPECT_GT(facts.memory_available, 0u);
  EXPECT_GT(facts.fds_open, 0u);
  EXPECT_GT(facts.fd_soft_limit, facts.fds_open);
}

TEST_F(EnvWindowsTest, NowMsDoesNotGoBackward) {
  const std::uint64_t first = env_.NowMs();
  const std::uint64_t second = env_.NowMs();
  EXPECT_GE(second, first);
}

}  // namespace
