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

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "cdn/cdn_env.h"
#include "cdn/env_windows.h"
#include "runtime/tests/loopback_http_server.h"

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
constexpr std::size_t kChunkBytes = 1024;
constexpr std::size_t kDripBytes = 64;
constexpr DWORD kDripIntervalMs = 250;
constexpr std::size_t kChildAppendBytes = 15;
constexpr std::size_t kVerifiedBytes = 8;
constexpr DWORD kSelfPathChars = MAX_PATH * 2;
constexpr std::size_t kSmallJsonBytes = 10;
constexpr char kSmallJsonText[] = "0123456789";
constexpr std::size_t kSmallReadMax = 100;
constexpr long kHttpOk = 200;
constexpr long kHttpPartial = 206;
constexpr long kHttpNotFound = 404;
constexpr std::size_t kRequestsServed = 3;
constexpr int kHoldAsks = 3;
constexpr std::uint64_t kHoldBoundMs = 20000;
constexpr std::uint64_t kGiveUpFactor = 3;
constexpr std::uint64_t kTookSlackMs = 10000;
constexpr int kWinsockMajor = 2;
constexpr int kWinsockMinor = 2;
constexpr std::size_t kPatternStep = 7;
constexpr std::size_t kPatternAlphabet = 26;

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

std::wstring Wide(const std::string& utf8) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
  return out;
}

// The child half of AKilledAppenderLeavesItsBytesForTheNextOpen: appends, then the process is killed with the file open.
constexpr const char* kChildTarget = "NEVR_ENV_WINDOWS_CHILD_FILE";
constexpr DWORD kChildExitKilled = 7;
constexpr DWORD kChildExitFailed = 3;
constexpr DWORD kChildWaitMs = 30000;
constexpr char kChildFilter[] = "EnvWindowsChild.AppendsThenIsKilled";

// The loopback cases run plain http through an Env that admits it (the Quest's transport admits it the same way for its tests); the
// production Env is https only, and AnotherSchemeIsATransportFailure checks that.
constexpr bool kAllowPlainHttpForLoopback = true;
// Get's stall bound, as env_windows.cpp sets it (kStallSeconds = 30).
constexpr std::uint64_t kMsPerSecond = 1000;
constexpr std::uint64_t kStallSecondsMs = 30 * kMsPerSecond;
constexpr std::size_t kCapBytes = 100000;
constexpr std::size_t kMiB = std::size_t{1} << 20;
constexpr std::size_t kShaMillionBytes = 1000000;
constexpr std::size_t kShaOddStep = 65537;
constexpr std::size_t kShaSevenStep = 7;
constexpr const char* kShaTwoBlock = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
constexpr const char* kShaMillionA = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

loopback::Script Reply(int status, std::string body) {
  loopback::Script step;
  step.status = status;
  step.body = std::move(body);
  return step;
}

struct Received {
  std::string body;
  std::vector<long> statuses;
};

// Takes at most `cap` bytes, as the cargo core's own sinks do: a body past the cap ends the transfer.
nevr_cdn::HttpBody Into(Received* r, std::size_t cap = ~std::size_t{0}) {
  return [r, cap](long status, const void* data, std::size_t length) {
    if (length > cap - r->body.size()) return false;
    r->body.append(static_cast<const char*>(data), length);
    r->statuses.push_back(status);
    return true;
  };
}

std::string Pattern(std::size_t n) {
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i * kPatternStep) % kPatternAlphabet);
  return s;
}

// A live server under Get's low-speed floor: it answers one GET with a 1 MiB Content-Length and then sends 64 bytes every 250 ms
// (256 B/s) until the client leaves.
class DripServer {
 public:
  DripServer() {
    WSADATA data = {};
    started_ = WSAStartup(MAKEWORD(kWinsockMajor, kWinsockMinor), &data) == 0;
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(addr);
    if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener_, 1) != 0 || getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      return;
    }
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }
  ~DripServer() {
    stop_ = true;
    if (listener_ != INVALID_SOCKET) closesocket(listener_);
    if (thread_.joinable()) thread_.join();
    if (started_) WSACleanup();
  }
  DripServer(const DripServer&) = delete;
  DripServer& operator=(const DripServer&) = delete;

  std::string Base() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
  void Serve() {
    SOCKET client = accept(listener_, nullptr, nullptr);
    if (client == INVALID_SOCKET) return;
    std::string raw;
    char chunk[kChunkBytes];
    while (raw.find("\r\n\r\n") == std::string::npos) {
      const int n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) break;
      raw.append(chunk, static_cast<std::size_t>(n));
    }
    const std::string head = "HTTP/1.1 200 X\r\nContent-Length: 1048576\r\nConnection: close\r\n\r\n";
    send(client, head.data(), static_cast<int>(head.size()), 0);
    const std::string drip(kDripBytes, 'd');
    while (!stop_ && send(client, drip.data(), static_cast<int>(drip.size()), 0) == static_cast<int>(drip.size())) {
      Sleep(kDripIntervalMs);
    }
    closesocket(client);
  }

  bool started_ = false;
  SOCKET listener_ = INVALID_SOCKET;
  unsigned short port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

std::string Sha(nevr_cdn::Env& env, const std::string& data, std::size_t chunk) {
  std::unique_ptr<nevr_cdn::Hasher> h = env.NewHasher();
  for (std::size_t at = 0; at < data.size(); at += chunk) h->Update(data.data() + at, std::min(chunk, data.size() - at));
  return h->Hex();
}

TEST_F(EnvWindowsTest, Sha256MatchesKnownVectors) {
  EXPECT_EQ(Sha(env_, "", 1), kEmptySha256);
  EXPECT_EQ(Sha(env_, "abc", 1), kAbcSha256);
  EXPECT_EQ(Sha(env_, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", kShaSevenStep), kShaTwoBlock);
  EXPECT_EQ(Sha(env_, std::string(kShaMillionBytes, 'a'), kShaOddStep), kShaMillionA);
}

TEST_F(EnvWindowsTest, AnAppendFileIsAPlainFileOpenedForWritingAlone) {
  EXPECT_EQ(env_.OpenAppend(dir_), nullptr) << "a folder";
  EXPECT_EQ(env_.OpenAppend(dir_ + "\\missing\\member.bin"), nullptr);
  std::unique_ptr<nevr_cdn::AppendFile> file = env_.OpenAppend(Path("member.bin"));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(env_.OpenAppend(Path("member.bin")), nullptr) << "a second writer";
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  EXPECT_FALSE(env_.OpenBlob(Path("member.bin"), &handle, &size)) << "a held reader admits no writer, so it waits for none";
  const HANDLE reader = CreateFileW(Wide(Path("member.bin")).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  EXPECT_NE(reader, INVALID_HANDLE_VALUE) << "a reader that shares writing is welcome";
  if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
}

// Run only as the child of AKilledAppenderLeavesItsBytesForTheNextOpen (kChildFilter).
TEST(EnvWindowsChild, AppendsThenIsKilled) {
  const wchar_t* path = _wgetenv(Wide(kChildTarget).c_str());
  if (path == nullptr) GTEST_SKIP() << "the child half of AKilledAppenderLeavesItsBytesForTheNextOpen";
  nevr_cdn::WindowsEnv env;
  std::unique_ptr<nevr_cdn::AppendFile> file = env.OpenAppend(Utf8(path));
  if (file == nullptr || !file->Append("half of a piece", kChildAppendBytes)) TerminateProcess(GetCurrentProcess(), kChildExitFailed);
  TerminateProcess(GetCurrentProcess(), kChildExitKilled);
}

TEST_F(EnvWindowsTest, AKilledAppenderLeavesItsBytesForTheNextOpen) {
  const std::string path = Path("member.bin");
  {
    std::unique_ptr<nevr_cdn::AppendFile> file = env_.OpenAppend(path);
    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(file->Append("verified", kVerifiedBytes));
    ASSERT_TRUE(file->Sync());
  }
  wchar_t self[kSelfPathChars] = {};
  ASSERT_GT(GetModuleFileNameW(nullptr, self, kSelfPathChars), 0u);
  std::wstring command = L"\"" + std::wstring(self) + L"\" --gtest_filter=" + Wide(kChildFilter);
  ASSERT_TRUE(SetEnvironmentVariableW(Wide(kChildTarget).c_str(), Wide(path).c_str()));
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child = {};
  const BOOL started = CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
  SetEnvironmentVariableW(Wide(kChildTarget).c_str(), nullptr);
  ASSERT_TRUE(started);
  ASSERT_EQ(WaitForSingleObject(child.hProcess, kChildWaitMs), WAIT_OBJECT_0);
  DWORD exit_code = 0;
  GetExitCodeProcess(child.hProcess, &exit_code);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  ASSERT_EQ(exit_code, kChildExitKilled);

  // The killed writer held no lock past its death, and the next open cuts its unverified bytes back.
  std::unique_ptr<nevr_cdn::AppendFile> file = env_.OpenAppend(path);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->Size(), kVerifiedBytes + kChildAppendBytes);
  ASSERT_TRUE(file->Truncate(kVerifiedBytes));
  file.reset();
  std::string text;
  ASSERT_TRUE(env_.ReadSmallFile(path, &text, kSmallFileLimit));
  EXPECT_EQ(text, "verified");
}

// A held target is never replaced: the replace is refused whole, the old file stays and the temp file goes.
TEST_F(EnvWindowsTest, WriteFileAtomicallyRefusesAHeldTarget) {
  const std::string target = Path("index.json");
  ASSERT_TRUE(env_.WriteFileAtomically(target, "new contents"));
  const HANDLE reader = CreateFileW(Wide(target).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(reader, INVALID_HANDLE_VALUE);
  EXPECT_FALSE(env_.WriteFileAtomically(target, "refused"));
  CloseHandle(reader);
  std::string text;
  ASSERT_TRUE(env_.ReadSmallFile(target, &text, kSmallFileLimit));
  EXPECT_EQ(text, "new contents");
  EXPECT_EQ(GetFileAttributesW(Wide(target + ".tmp").c_str()), INVALID_FILE_ATTRIBUTES) << "the temp file is removed";

  EXPECT_FALSE(env_.WriteFileAtomically(dir_ + "\\missing\\index.json", "x"));
  EXPECT_FALSE(env_.WriteFileAtomically(dir_ + "\\", "x"));
  ASSERT_TRUE(env_.ReadSmallFile(target, &text, kSmallFileLimit));
  EXPECT_EQ(text, "new contents");
}

// The refusals the file seams make: a size cap, a folder, a missing file, a relative folder, a path that is a file.
TEST_F(EnvWindowsTest, FilesRefuseWhatThePosixEnvRefuses) {
  std::string text;
  ASSERT_TRUE(env_.WriteFileAtomically(Path("small.json"), kSmallJsonText));
  EXPECT_FALSE(env_.ReadSmallFile(Path("small.json"), &text, kSmallJsonBytes - 1));
  EXPECT_TRUE(env_.ReadSmallFile(Path("small.json"), &text, kSmallJsonBytes));
  EXPECT_EQ(text, kSmallJsonText);
  EXPECT_FALSE(env_.ReadSmallFile(Path("absent.json"), &text, kSmallReadMax));
  EXPECT_FALSE(env_.ReadSmallFile(dir_, &text, kSmallReadMax));  // a folder

  std::int64_t handle = 0;
  std::uint64_t size = 0;
  EXPECT_FALSE(env_.OpenBlob(dir_, &handle, &size));
  EXPECT_FALSE(env_.OpenBlob(Path("absent.blob"), &handle, &size));
  EXPECT_FALSE(env_.MakeDirs("relative\\dir"));

  EXPECT_TRUE(env_.MakeDirs(dir_ + "/a/b\\c"));
  EXPECT_TRUE(env_.MakeDirs(dir_ + "/a/b\\c"));
  ASSERT_TRUE(env_.WriteFileAtomically(dir_ + "/a/b/c/x.json", "{}"));
  EXPECT_FALSE(env_.MakeDirs(dir_ + "/a/b/c/x.json"));
}

// The device facts when the folder is not there: no free-space fact, the others still come.
TEST_F(EnvWindowsTest, QueryDeviceWithAnAbsentFolderKeepsTheOtherFacts) {
  nevr_cdn::DeviceFacts facts;
  ASSERT_TRUE(env_.QueryDevice(dir_ + "\\absent\\deeper", &facts));
  EXPECT_FALSE(facts.have_free);
  EXPECT_EQ(facts.free_bytes, 0u);
  EXPECT_TRUE(facts.have_fds);
  EXPECT_TRUE(facts.have_memory);
}

TEST(EnvWindowsHttp, AGetStopsAtTheCap) {
  const std::string big = Pattern(kMiB);
  loopback::LoopbackServer server({Reply(kHttpOk, big), Reply(kHttpOk, big), Reply(kHttpNotFound, "nope")});
  nevr_cdn::WindowsEnv env(nullptr, kAllowPlainHttpForLoopback);

  Received capped;
  const nevr_cdn::HttpResult stopped = env.Get(server.Base() + "/piece", Into(&capped, kCapBytes), [] { return true; });
  EXPECT_FALSE(stopped.completed);
  EXPECT_FALSE(stopped.cancelled);
  EXPECT_EQ(stopped.code, static_cast<int>(CURLE_WRITE_ERROR));
  EXPECT_EQ(stopped.status, kHttpOk);
  EXPECT_LE(capped.body.size(), kCapBytes);
  EXPECT_EQ(capped.body, big.substr(0, capped.body.size()));

  Received whole;
  const nevr_cdn::HttpResult done = env.Get(server.Base() + "/piece", Into(&whole, big.size()), [] { return true; });
  EXPECT_TRUE(done.completed);
  EXPECT_EQ(done.code, 0);
  EXPECT_EQ(whole.body, big);

  Received missing;
  const nevr_cdn::HttpResult notFound = env.Get(server.Base() + "/manifest.json", Into(&missing), [] { return true; });
  EXPECT_TRUE(notFound.completed);
  EXPECT_EQ(notFound.status, kHttpNotFound);
  EXPECT_EQ(missing.body, "nope");
  ASSERT_EQ(missing.statuses.size(), 1u);
  EXPECT_EQ(missing.statuses[0], kHttpNotFound);

  ASSERT_TRUE(server.WaitForRequests(kRequestsServed));
  EXPECT_EQ(server.Requests()[2].request_line, "GET /manifest.json HTTP/1.1");
}

// A range answer (206) is a body like any other: Get hands it on with its status and says the transfer completed.
TEST(EnvWindowsHttp, APartialContentAnswerIsDeliveredWithItsStatus) {
  loopback::LoopbackServer server({Reply(kHttpPartial, "the second half")});
  nevr_cdn::WindowsEnv env(nullptr, kAllowPlainHttpForLoopback);
  Received r;
  const nevr_cdn::HttpResult result = env.Get(server.Base() + "/piece", Into(&r), [] { return true; });
  EXPECT_TRUE(result.completed);
  EXPECT_EQ(result.status, kHttpPartial);
  EXPECT_EQ(result.code, 0);
  EXPECT_EQ(r.body, "the second half");
  ASSERT_FALSE(r.statuses.empty());
  EXPECT_EQ(r.statuses[0], kHttpPartial);
}

TEST(EnvWindowsHttp, KeepGoingEndsAHeldTransfer) {
  loopback::Script hold;
  hold.kind = loopback::Script::Kind::Hold;
  loopback::LoopbackServer server({hold});
  nevr_cdn::WindowsEnv env(nullptr, kAllowPlainHttpForLoopback);
  const std::uint64_t start = env.NowMs();
  Received r;
  int asked = 0;
  const nevr_cdn::HttpResult result = env.Get(server.Base() + "/piece", Into(&r), [&] { return ++asked < kHoldAsks; });
  EXPECT_TRUE(result.cancelled);
  EXPECT_FALSE(result.completed);
  EXPECT_TRUE(r.body.empty());
  EXPECT_LT(env.NowMs() - start, kHoldBoundMs);
}

// A body that keeps arriving, but under the floor, ends at the stall bound, so a fetch worker's life has a bound while the server
// stays connected.
TEST(EnvWindowsHttp, ATransferUnderTheLowSpeedFloorTimesOut) {
  DripServer server;
  nevr_cdn::WindowsEnv env(nullptr, kAllowPlainHttpForLoopback);
  const std::uint64_t start = env.NowMs();
  const std::uint64_t giveUpMs = kGiveUpFactor * kStallSecondsMs;
  Received r;
  const nevr_cdn::HttpResult result = env.Get(server.Base() + "/piece", Into(&r), [&] { return env.NowMs() - start < giveUpMs; });
  const std::uint64_t took = env.NowMs() - start;
  EXPECT_EQ(result.code, static_cast<int>(CURLE_OPERATION_TIMEDOUT));
  EXPECT_FALSE(result.completed);
  EXPECT_FALSE(result.cancelled);
  EXPECT_EQ(result.status, kHttpOk);
  EXPECT_FALSE(r.body.empty()) << "the server was sending";
  EXPECT_GE(took, kStallSecondsMs - kMsPerSecond);
  EXPECT_LT(took, kStallSecondsMs + kTookSlackMs);
}

// Production Get is https only: plain http and a file URL are transport failures that never reach the network.
TEST(EnvWindowsHttp, AnotherSchemeIsATransportFailure) {
  nevr_cdn::WindowsEnv strict(nullptr);
  Received r;
  const nevr_cdn::HttpResult plain = strict.Get("http://127.0.0.1:1/piece", Into(&r), [] { return true; });
  EXPECT_FALSE(plain.completed);
  EXPECT_EQ(plain.status, 0);
  EXPECT_EQ(plain.code, static_cast<int>(CURLE_UNSUPPORTED_PROTOCOL));
  const nevr_cdn::HttpResult refused = strict.Get("file:///C:/Windows/win.ini", Into(&r), [] { return true; });
  EXPECT_FALSE(refused.completed);
  EXPECT_EQ(refused.code, static_cast<int>(CURLE_UNSUPPORTED_PROTOCOL));
  EXPECT_TRUE(r.body.empty());
}

TEST(EnvWindowsHttp, AnUnreachableHostIsATransportFailure) {
  nevr_cdn::WindowsEnv env(nullptr, kAllowPlainHttpForLoopback);
  Received r;
  const nevr_cdn::HttpResult result = env.Get("http://127.0.0.1:1/piece", Into(&r), [] { return true; });
  EXPECT_FALSE(result.completed);
  EXPECT_EQ(result.status, 0);
  EXPECT_NE(result.code, 0);
  EXPECT_TRUE(r.body.empty());
}

}  // namespace
