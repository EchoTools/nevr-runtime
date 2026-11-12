// The PC's nevr_cargo::Env and nevr_maps::MapFs (runtime/cargo/win_env.h), driven for real: Win32 files in a temp tree, BCrypt, and
// libcurl against the scripted loopback server. No test reaches the internet.
#include "runtime/cargo/win_env.h"

#include <curl/curl.h>
#include <gtest/gtest.h>

#include "runtime/tests/loopback_http_server.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/cargo_store.h"
#include "core/logging.h"
#include "core/nevr_root.h"

namespace {

std::mutex g_logMutex;
std::vector<std::string> g_logLines;

}  // namespace

VOID Log(EchoVR::LogLevel, const CHAR* format, ...) {
  char line[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_logLines.emplace_back(line);
}

namespace {

namespace nc = nevr_cargo;
namespace nm = nevr_maps;

constexpr const char* kChildTarget = "NEVR_WIN_ENV_CHILD_DIR";

std::string Utf8(const std::wstring& wide) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
  return out;
}

std::wstring Wide(const std::string& utf8) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
  return out;
}

// A fresh folder under %TEMP% with a non-ASCII name, so every path crosses the UTF-8 to UTF-16 conversion.
class WinEnvTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<int> serial{0};
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    ASSERT_GT(n, 0u);
    dir_ = Utf8(std::wstring(temp, n)) + "nevr_win_env_\xc3\xa9_" + std::to_string(GetCurrentProcessId()) + "_" +
           std::to_string(serial.fetch_add(1));
    env_ = nc::MakeWinEnv("[NEVR.MAPS]");
    fs_ = nm::MakeWinMapFs();
    ASSERT_TRUE(fs_->RemoveTree(dir_));
    ASSERT_TRUE(env_->MakeDirs(dir_));
  }
  void TearDown() override { fs_->RemoveTree(dir_); }

  std::string Path(const char* name) const { return dir_ + "\\" + name; }

  std::string Read(const std::string& path) {
    std::string text;
    EXPECT_TRUE(env_->ReadSmallFile(path, &text, 1 << 20)) << path;
    return text;
  }

  std::size_t Parts() {
    std::vector<std::string> names;
    EXPECT_TRUE(fs_->ListDir(dir_, &names));
    std::size_t parts = 0;
    for (const std::string& name : names) {
      if (name.size() == 22 && name[0] == '~' && name.compare(17, 5, ".part") == 0) ++parts;
    }
    return parts;
  }

  nm::FileStamp StampNow(const std::string& path) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    nm::FileStamp stamp;
    EXPECT_TRUE(fs_->OpenHeld(path, &handle, &size));
    EXPECT_TRUE(fs_->StampOf(handle, &stamp));
    env_->CloseBlob(handle);
    return stamp;
  }

  std::string dir_;
  std::unique_ptr<nc::Env> env_;
  std::unique_ptr<nm::MapFs> fs_;
};

std::string Sha(nc::Env& env, const std::string& data, std::size_t chunk) {
  std::unique_ptr<nc::Hasher> h = env.NewHasher();
  for (std::size_t at = 0; at < data.size(); at += chunk) h->Update(data.data() + at, std::min(chunk, data.size() - at));
  return h->Hex();
}

TEST_F(WinEnvTest, AHeldFileRefusesAWriterAndADelete) {
  const std::string blob = Path("held.blob");
  ASSERT_TRUE(env_->WriteFileAtomically(blob, "mounted bytes"));
  for (const bool through_map_fs : {false, true}) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    ASSERT_TRUE(through_map_fs ? fs_->OpenHeld(blob, &handle, &size) : env_->OpenBlob(blob, &handle, &size));
    EXPECT_EQ(size, 13u);

    const std::wstring wide = Wide(blob);
    const HANDLE writer = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    EXPECT_FALSE(DeleteFileW(wide.c_str()));
    EXPECT_FALSE(env_->WriteFileAtomically(blob, "replacement"));  // a rename over it is refused too
    EXPECT_EQ(Parts(), 0u);

    const HANDLE reader = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(reader, INVALID_HANDLE_VALUE);  // other readers are welcome
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);

    char buffer[64] = {};
    std::size_t got = 0;
    ASSERT_TRUE(env_->ReadBlob(handle, 8, buffer, sizeof(buffer), &got));
    EXPECT_EQ(std::string(buffer, got), "bytes");
    ASSERT_TRUE(env_->ReadBlob(handle, 100, buffer, sizeof(buffer), &got));
    EXPECT_EQ(got, 0u);
    env_->CloseBlob(handle);
  }
  EXPECT_TRUE(DeleteFileW(Wide(blob).c_str()));
}

TEST_F(WinEnvTest, CommitReplacesTheFileInOneStep) {
  const std::string target = Path("index.json");
  ASSERT_TRUE(env_->WriteFileAtomically(target, "old"));

  std::unique_ptr<nc::TempFile> temp = env_->CreateTemp(dir_);
  ASSERT_NE(temp, nullptr);
  ASSERT_TRUE(temp->Write("new ", 4));
  ASSERT_TRUE(temp->Write("contents", 8));
  EXPECT_EQ(Read(target), "old");
  EXPECT_EQ(Parts(), 1u);
  ASSERT_TRUE(temp->Commit(target));
  EXPECT_EQ(Read(target), "new contents");
  EXPECT_EQ(Parts(), 0u);
  EXPECT_FALSE(temp->Commit(target));  // a temp commits once

  // While anyone has the file open the replace is refused whole: the old file stays, the temp goes.
  const HANDLE reader = CreateFileW(Wide(target).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(reader, INVALID_HANDLE_VALUE);
  EXPECT_FALSE(env_->WriteFileAtomically(target, "refused"));
  CloseHandle(reader);
  EXPECT_EQ(Read(target), "new contents");
  EXPECT_EQ(Parts(), 0u);

  std::unique_ptr<nc::TempFile> aborted = env_->CreateTemp(dir_);
  ASSERT_NE(aborted, nullptr);
  ASSERT_TRUE(aborted->Write("discarded", 9));
  aborted->Abort();
  EXPECT_EQ(Parts(), 0u);
  EXPECT_FALSE(aborted->Write("x", 1));

  {
    std::unique_ptr<nc::TempFile> dropped = env_->CreateTemp(dir_);
    ASSERT_NE(dropped, nullptr);
    ASSERT_TRUE(dropped->Write("dropped", 7));
  }
  EXPECT_EQ(Parts(), 0u);

  std::unique_ptr<nc::TempFile> nowhere = env_->CreateTemp(dir_);
  ASSERT_NE(nowhere, nullptr);
  EXPECT_FALSE(nowhere->Commit(dir_ + "\\missing\\index.json"));
  EXPECT_EQ(Parts(), 0u);
  EXPECT_EQ(Read(target), "new contents");
}

// Run only as the child of AKilledWriterLeavesTheOldFile: writes part of a replacement, then the process is killed mid-write.
TEST(WinEnvChild, WritesThenIsKilled) {
  const wchar_t* dir = _wgetenv(Wide(kChildTarget).c_str());  // wide: the folder name is not in the ANSI code page
  if (dir == nullptr) GTEST_SKIP() << "the child half of AKilledWriterLeavesTheOldFile";
  std::unique_ptr<nc::Env> env = nc::MakeWinEnv("[NEVR.MAPS]");
  std::unique_ptr<nc::TempFile> temp = env->CreateTemp(Utf8(dir));
  if (temp == nullptr || !temp->Write("half of the new", 15)) TerminateProcess(GetCurrentProcess(), 3);
  TerminateProcess(GetCurrentProcess(), 7);
}

TEST_F(WinEnvTest, AKilledWriterLeavesTheOldFile) {
  const std::string target = Path("index.json");
  ASSERT_TRUE(env_->WriteFileAtomically(target, "old and whole"));

  wchar_t self[MAX_PATH * 2] = {};
  ASSERT_GT(GetModuleFileNameW(nullptr, self, MAX_PATH * 2), 0u);
  std::wstring command = L"\"" + std::wstring(self) + L"\" --gtest_filter=WinEnvChild.WritesThenIsKilled";
  ASSERT_TRUE(SetEnvironmentVariableW(Wide(kChildTarget).c_str(), Wide(dir_).c_str()));
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child = {};
  const BOOL started = CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
  SetEnvironmentVariableW(Wide(kChildTarget).c_str(), nullptr);
  ASSERT_TRUE(started);
  ASSERT_EQ(WaitForSingleObject(child.hProcess, 30000), WAIT_OBJECT_0);
  DWORD exit_code = 0;
  GetExitCodeProcess(child.hProcess, &exit_code);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  ASSERT_EQ(exit_code, 7u);

  EXPECT_EQ(Read(target), "old and whole");
  EXPECT_EQ(Parts(), 1u);  // the killed writer's .part is left for the GC
  EXPECT_TRUE(fs_->RemoveTree(dir_));
}

TEST_F(WinEnvTest, Sha256MatchesKnownVectors) {
  EXPECT_EQ(Sha(*env_, "", 1), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(Sha(*env_, "abc", 1), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(Sha(*env_, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 7),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(Sha(*env_, std::string(1000000, 'a'), 65537), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

  std::unique_ptr<nc::Hasher> h = env_->NewHasher();
  h->Update("abc", 3);
  EXPECT_EQ(h->Hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(h->Hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");  // reset after Hex

  // The cargo core's own file hash, through this env's blob reads.
  const std::string blob = Path("abc.blob");
  ASSERT_TRUE(env_->WriteFileAtomically(blob, "abc"));
  EXPECT_EQ(nc::HashFile(*env_, blob), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_F(WinEnvTest, StampOfChangesOnARewriteAndIsStableOnAReopen) {
  const std::string path = Path("map.blob");
  ASSERT_TRUE(env_->WriteFileAtomically(path, "version one"));
  const nm::FileStamp first = StampNow(path);
  EXPECT_EQ(first.size, 11u);
  EXPECT_NE(first.last_write, 0u);
  EXPECT_TRUE(StampNow(path) == first);

  ASSERT_TRUE(env_->WriteFileAtomically(path, "version one"));  // same bytes, a new file renamed over it
  const nm::FileStamp replaced = StampNow(path);
  EXPECT_FALSE(replaced == first);
  EXPECT_TRUE(replaced.id_hi != first.id_hi || replaced.id_lo != first.id_lo);

  const HANDLE in_place = CreateFileW(Wide(path).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(in_place, INVALID_HANDLE_VALUE);
  DWORD wrote = 0;
  ASSERT_TRUE(WriteFile(in_place, "version two, longer", 19, &wrote, nullptr));
  CloseHandle(in_place);
  const nm::FileStamp rewritten = StampNow(path);
  EXPECT_FALSE(rewritten == replaced);
  EXPECT_EQ(rewritten.size, 19u);
  EXPECT_EQ(rewritten.id_lo, replaced.id_lo);

  nm::FileStamp none;
  EXPECT_FALSE(fs_->StampOf(reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE), &none));
}

TEST_F(WinEnvTest, FinalPathOfNamesTheOpenFileInUtf8) {
  const std::string path = Path("final.blob");
  ASSERT_TRUE(env_->WriteFileAtomically(path, "x"));
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  ASSERT_TRUE(fs_->OpenHeld(path, &handle, &size));
  const std::string final_path = fs_->FinalPathOf(handle);
  env_->CloseBlob(handle);
  ASSERT_FALSE(final_path.empty());
  EXPECT_NE(final_path.rfind("\\\\?\\", 0), 0u);
  EXPECT_EQ(final_path[1], ':');
  EXPECT_NE(final_path.find("nevr_win_env_\xc3\xa9_"), std::string::npos);
  EXPECT_EQ(final_path.substr(final_path.size() - 11), "\\final.blob");
  EXPECT_TRUE(fs_->FinalPathOf(reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE)).empty());
}

TEST_F(WinEnvTest, RemoveTreeIsRefusedWhileAFileInsideIsHeld) {
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\tree\\set"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "\\tree\\package.json", "{}"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "\\tree\\set\\windows-x86_64.nevrres", "set"));
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  ASSERT_TRUE(fs_->OpenHeld(dir_ + "\\tree\\set\\windows-x86_64.nevrres", &handle, &size));
  EXPECT_FALSE(fs_->RemoveTree(dir_ + "\\tree"));
  std::string text;
  EXPECT_TRUE(env_->ReadSmallFile(dir_ + "\\tree\\set\\windows-x86_64.nevrres", &text, 100));
  env_->CloseBlob(handle);
  EXPECT_TRUE(fs_->RemoveTree(dir_ + "\\tree\\"));
  EXPECT_EQ(GetFileAttributesW(Wide(dir_ + "\\tree").c_str()), INVALID_FILE_ATTRIBUTES);
  EXPECT_TRUE(fs_->RemoveTree(dir_ + "\\tree"));  // already gone
  EXPECT_FALSE(fs_->RemoveTree("relative\\tree"));
  ASSERT_TRUE(env_->WriteFileAtomically(Path("file"), "x"));
  EXPECT_FALSE(fs_->RemoveTree(Path("file")));
}

TEST_F(WinEnvTest, RemoveTreeRemovesALinkNotItsTarget) {
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\outside"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "\\outside\\keep.txt", "keep"));
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\tree"));
  const std::wstring command = L"mklink /J \"" + Wide(dir_ + "\\tree\\link") + L"\" \"" + Wide(dir_ + "\\outside") + L"\" >nul";
  ASSERT_EQ(_wsystem(command.c_str()), 0);
  std::string text;
  ASSERT_TRUE(env_->ReadSmallFile(dir_ + "\\tree\\link\\keep.txt", &text, 100));

  EXPECT_TRUE(fs_->RemoveTree(dir_ + "\\tree"));
  EXPECT_TRUE(env_->ReadSmallFile(dir_ + "\\outside\\keep.txt", &text, 100));
  EXPECT_EQ(text, "keep");
}

TEST_F(WinEnvTest, ListDirAndRenameDir) {
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\a.tmp\\scripts"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "\\a.tmp\\package.json", "{}"));
  std::vector<std::string> names;
  ASSERT_TRUE(fs_->ListDir(dir_ + "\\a.tmp", &names));
  EXPECT_EQ(names, (std::vector<std::string>{"package.json", "scripts"}));
  ASSERT_TRUE(fs_->ListDir(dir_ + "\\a.tmp\\scripts\\", &names));
  EXPECT_TRUE(names.empty());
  EXPECT_FALSE(fs_->ListDir(dir_ + "\\absent", &names));

  ASSERT_TRUE(fs_->RenameDir(dir_ + "\\a.tmp", dir_ + "\\a"));
  std::string text;
  EXPECT_TRUE(env_->ReadSmallFile(dir_ + "\\a\\package.json", &text, 100));
  ASSERT_TRUE(env_->MakeDirs(dir_ + "\\b"));
  EXPECT_FALSE(fs_->RenameDir(dir_ + "\\b", dir_ + "\\a"));  // never over an existing folder
  EXPECT_FALSE(fs_->RenameDir(dir_ + "\\absent", dir_ + "\\c"));
}

TEST_F(WinEnvTest, FilesRefuseWhatThePosixEnvRefuses) {
  std::string text;
  ASSERT_TRUE(env_->WriteFileAtomically(Path("small.json"), "0123456789"));
  EXPECT_FALSE(env_->ReadSmallFile(Path("small.json"), &text, 9));
  EXPECT_TRUE(env_->ReadSmallFile(Path("small.json"), &text, 10));
  EXPECT_EQ(text, "0123456789");
  EXPECT_FALSE(env_->ReadSmallFile(Path("absent.json"), &text, 100));
  EXPECT_FALSE(env_->ReadSmallFile(dir_, &text, 100));  // a folder
  EXPECT_FALSE(env_->ReadSmallFile("small.json", &text, 100));  // relative
  EXPECT_FALSE(env_->ReadSmallFile(Path("small.json") + std::string(1, '\0') + "x", &text, 100));

  std::int64_t handle = 0;
  std::uint64_t size = 0;
  EXPECT_FALSE(env_->OpenBlob(dir_, &handle, &size));
  EXPECT_FALSE(env_->OpenBlob(Path("absent.blob"), &handle, &size));
  EXPECT_FALSE(env_->MakeDirs("relative\\dir"));
  EXPECT_EQ(env_->CreateTemp("relative"), nullptr);
  EXPECT_FALSE(env_->WriteFileAtomically("nofolder", "x"));

  EXPECT_TRUE(env_->MakeDirs(dir_ + "/a/b\\c"));
  EXPECT_TRUE(env_->MakeDirs(dir_ + "/a/b\\c"));
  ASSERT_TRUE(env_->WriteFileAtomically(dir_ + "/a/b/c/x.json", "{}"));
  EXPECT_FALSE(env_->MakeDirs(dir_ + "/a/b/c/x.json"));
}

TEST_F(WinEnvTest, TheCargoStoreRunsOnTheMapsCache) {
  const std::string maps = nevr_root::MapsCacheDir(dir_);
  EXPECT_EQ(maps, dir_ + "\\cache\\maps");
  ASSERT_TRUE(env_->MakeDirs(maps));

  nc::Index in;
  nc::IndexEntry e;
  e.type = 0xbeac1969cb7b8861ull;
  e.name = 0x1234;
  e.sha256 = Sha(*env_, "blob", 4);
  e.label = "maps";
  e.format = "zstd-frame";
  e.bytes = 4;
  e.align = 16;
  in.entries.push_back(e);
  ASSERT_TRUE(nc::WriteIndex(*env_, maps, in));
  nc::Index out;
  ASSERT_EQ(nc::ReadIndex(*env_, maps, &out), nc::IndexStatus::kOk);
  EXPECT_EQ(out, in);
}

TEST(WinEnvPaths, MapsCacheDirIsUnderTheCache) {
  EXPECT_EQ(nevr_root::MapsCacheDir(std::wstring(L"C:\\L\\nevr\\echovr")), L"C:\\L\\nevr\\echovr\\cache\\maps");
  EXPECT_EQ(nevr_root::MapsCacheDir(std::string("C:\\L\\nevr\\echovr\\")), "C:\\L\\nevr\\echovr\\cache\\maps");
  EXPECT_TRUE(nevr_root::MapsCacheDir(std::string()).empty());
}

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
nc::HttpBody Into(Received* r, std::size_t cap = ~std::size_t{0}) {
  return [r, cap](long status, const void* data, std::size_t length) {
    if (length > cap - r->body.size()) return false;
    r->body.append(static_cast<const char*>(data), length);
    r->statuses.push_back(status);
    return true;
  };
}

std::string Pattern(std::size_t n) {
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i * 7) % 26);
  return s;
}

TEST(WinEnvHttp, AGetStopsAtTheCap) {
  const std::string big = Pattern(1 << 20);
  loopback::LoopbackServer server({Reply(200, big), Reply(200, big), Reply(404, "nope")});
  std::unique_ptr<nc::Env> env = nc::MakeWinEnv("[NEVR.MAPS]");

  Received capped;
  const nc::HttpResult stopped = env->Get(server.Base() + "/piece", Into(&capped, 100000), [] { return true; });
  EXPECT_FALSE(stopped.completed);
  EXPECT_FALSE(stopped.cancelled);
  EXPECT_EQ(stopped.code, static_cast<int>(CURLE_WRITE_ERROR));
  EXPECT_EQ(stopped.status, 200);
  EXPECT_LE(capped.body.size(), 100000u);
  EXPECT_EQ(capped.body, big.substr(0, capped.body.size()));

  Received whole;
  const nc::HttpResult done = env->Get(server.Base() + "/piece", Into(&whole, big.size()), [] { return true; });
  EXPECT_TRUE(done.completed);
  EXPECT_EQ(done.code, 0);
  EXPECT_EQ(whole.body, big);

  Received missing;
  const nc::HttpResult notFound = env->Get(server.Base() + "/manifest.json", Into(&missing), [] { return true; });
  EXPECT_TRUE(notFound.completed);
  EXPECT_EQ(notFound.status, 404);
  EXPECT_EQ(missing.body, "nope");
  ASSERT_EQ(missing.statuses.size(), 1u);
  EXPECT_EQ(missing.statuses[0], 404);

  ASSERT_TRUE(server.WaitForRequests(3));
  EXPECT_EQ(server.Requests()[2].request_line, "GET /manifest.json HTTP/1.1");
}

TEST(WinEnvHttp, KeepGoingEndsAHeldTransfer) {
  loopback::Script hold;
  hold.kind = loopback::Script::Kind::Hold;
  loopback::LoopbackServer server({hold});
  std::unique_ptr<nc::Env> env = nc::MakeWinEnv("[NEVR.MAPS]");
  const std::uint64_t start = env->NowMs();
  Received r;
  int asked = 0;
  const nc::HttpResult result = env->Get(server.Base() + "/piece", Into(&r), [&] { return ++asked < 3; });
  EXPECT_TRUE(result.cancelled);
  EXPECT_FALSE(result.completed);
  EXPECT_TRUE(r.body.empty());
  EXPECT_LT(env->NowMs() - start, 20000u);
}

// A live server under Get's low-speed floor: it answers one GET with a 1 MiB Content-Length and then sends 64 bytes every
// 250 ms (256 B/s) until the client leaves.
class DripServer {
 public:
  DripServer() {
    WSADATA data = {};
    started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
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
  std::size_t Sent() const { return sent_.load(); }

 private:
  void Serve() {
    SOCKET client = accept(listener_, nullptr, nullptr);
    if (client == INVALID_SOCKET) return;
    std::string raw;
    char chunk[1024];
    while (raw.find("\r\n\r\n") == std::string::npos) {
      const int n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) break;
      raw.append(chunk, static_cast<std::size_t>(n));
    }
    const std::string head = "HTTP/1.1 200 X\r\nContent-Length: 1048576\r\nConnection: close\r\n\r\n";
    send(client, head.data(), static_cast<int>(head.size()), 0);
    const std::string drip(64, 'd');
    while (!stop_ && send(client, drip.data(), static_cast<int>(drip.size()), 0) == static_cast<int>(drip.size())) {
      sent_ += drip.size();
      Sleep(250);
    }
    closesocket(client);
  }

  bool started_ = false;
  SOCKET listener_ = INVALID_SOCKET;
  unsigned short port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> sent_{0};
  std::thread thread_;
};

// A body that keeps arriving, but under the floor, ends at the low-speed bound: the old 1 B/s floor never ended it, so a
// fetch worker's life had no bound while the server stayed connected (CDN_DESIGN finding 17).
TEST(WinEnvHttp, ATransferUnderTheLowSpeedFloorTimesOut) {
  DripServer server;
  std::unique_ptr<nc::Env> env = nc::MakeWinEnv("[NEVR.MAPS]");
  const std::uint64_t start = env->NowMs();
  const std::uint64_t giveUpMs = 3 * nc::kLowSpeedSeconds * 1000;
  Received r;
  const nc::HttpResult result =
      env->Get(server.Base() + "/piece", Into(&r), [&] { return env->NowMs() - start < giveUpMs; });
  const std::uint64_t took = env->NowMs() - start;
  EXPECT_EQ(result.code, static_cast<int>(CURLE_OPERATION_TIMEDOUT));
  EXPECT_FALSE(result.completed);
  EXPECT_FALSE(result.cancelled);
  EXPECT_EQ(result.status, 200);
  EXPECT_FALSE(r.body.empty()) << "the server was sending";
  EXPECT_GE(took, (nc::kLowSpeedSeconds - 1) * 1000u);
  EXPECT_LT(took, (nc::kLowSpeedSeconds + 10) * 1000u);
}

TEST(WinEnvHttp, TheTransportBoundsAreTheDesignsValues) {
  EXPECT_EQ(nc::kConnectTimeoutSeconds, 10);
  EXPECT_EQ(nc::kLowSpeedBytesPerSecond, 1024);
  EXPECT_EQ(nc::kLowSpeedSeconds, 15);
}

TEST(WinEnvHttp, AnUnreachableHostOrAnotherSchemeIsATransportFailure) {
  std::unique_ptr<nc::Env> env = nc::MakeWinEnv("[NEVR.MAPS]");
  Received r;
  const nc::HttpResult result = env->Get("http://127.0.0.1:1/piece", Into(&r), [] { return true; });
  EXPECT_FALSE(result.completed);
  EXPECT_EQ(result.status, 0);
  EXPECT_NE(result.code, 0);
  const nc::HttpResult refused = env->Get("file:///C:/Windows/win.ini", Into(&r), [] { return true; });
  EXPECT_FALSE(refused.completed);
  EXPECT_EQ(refused.code, static_cast<int>(CURLE_UNSUPPORTED_PROTOCOL));
  EXPECT_TRUE(r.body.empty());
}

TEST(WinEnvEvents, AnEventIsOnePrefixedLine) {
  std::unique_ptr<nc::Env> env = nc::MakeWinEnv("[NEVR.MAPS]");
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logLines.clear();
  }
  env->Event(nc::EventLevel::kWarn, "maps_fetch_failed",
             {{"reason", "over_cap"}, {"bytes", 42}, {"detail", "two words"}, {"empty", ""}, {"q", "a\"b"}});
  std::lock_guard<std::mutex> lock(g_logMutex);
  ASSERT_EQ(g_logLines.size(), 1u);
  EXPECT_EQ(g_logLines[0], "[NEVR.MAPS] maps_fetch_failed reason=over_cap bytes=42 detail=\"two words\" empty=\"\" q=\"a\\\"b\"");
}

}  // namespace
