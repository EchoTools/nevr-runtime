// Self-update's swap at start (docs/adr/0024-pc-self-update.md, section 4; #2104), over real files in a temporary nevr
// root, under Wine. The release files are the signed fixtures tools/gen_plugin_signature_fixtures.py makes into the
// directory NEVR_SIGFIX_DIR names (`just test-auth-unit` makes it): `nevr-runtime.signed` (the rendezvous runtime
// stand-in, signed by the throwaway PKI, NEVR-BUILD 7.7.7) is the new runtime, `nevr-runtime.unsigned` (the same
// stand-in, unsigned) the one in the root, and `good` and `good2` two signed plugins. The tests fail without the
// directory: a swap test that skips is a swap nobody tested.
//
// Each test names the ADR 0024 failure case it covers (F6 to F23).

#include "core/runtime_abi.h"
#include "runtime/host/runtime_load.h"
#include "runtime/host/update_names.h"
#include "runtime/host/update_swap.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using nevr_host::BootstrapDiagnostics;
using nevr_host::RunUpdateAtStart;
using nevr_host::UpdateLock;
using nevr_host::UpdateAction;
using nevr_host::UpdateResult;
namespace names = nevr_host::update_names;

// ---- the vocabulary ----------------------------------------------------------------------------------------------

TEST(UpdateNames, AVersionIsAPlainNameThatCannotLeaveItsFolder) {
  for (const char* good : {"1.2.3", "6.16.5-dev.28+1b7ce30a", "A-b.c+9", "0"}) {
    EXPECT_TRUE(names::IsVersionName(good)) << good;
  }
  EXPECT_TRUE(names::IsVersionName(std::string(64, 'a')));
  for (const char* bad : {"", ".", "..", ".1", "1.", "1/2", "1\\2", "1 2", "1:2", "1_2", "CON", "con.1", "NUL", "COM1",
                          "lpt9.x", "Aux", "prn"}) {
    EXPECT_FALSE(names::IsVersionName(bad)) << bad;
  }
  EXPECT_FALSE(names::IsVersionName(std::string(65, 'a')));
}

TEST(UpdateNames, APluginNameIsAPlainNameThatCannotLeaveItsFolder) {
  for (const char* good : {"example", "ok-1_a", "a.b", "COMET", "console"}) {
    EXPECT_TRUE(names::IsPluginName(good)) << good;
  }
  EXPECT_TRUE(names::IsPluginName(std::string(64, 'p')));
  for (const char* bad : {"", ".", "..", ".x", "x.", "a/b", "a\\b", "a b", "a+b", "CON", "aux", "Com7", "nul.x",
                          "CONIN$", "conout$"}) {
    EXPECT_FALSE(names::IsPluginName(bad)) << bad;
  }
  EXPECT_FALSE(names::IsPluginName(std::string(65, 'p')));
}

TEST(UpdateNames, AMemberIsTheRuntimeOrOnePluginAndNothingElse) {
  names::Member member;
  ASSERT_TRUE(names::ParseMember("nevr-runtime.dll", &member));
  EXPECT_TRUE(member.runtime);
  EXPECT_EQ(names::MemberPath(member), "nevr-runtime.dll");
  EXPECT_EQ(names::MemberRelativePath(member), L"nevr-runtime.dll");

  ASSERT_TRUE(names::ParseMember("plugins/example.dll", &member));
  EXPECT_FALSE(member.runtime);
  EXPECT_EQ(member.plugin, "example");
  EXPECT_EQ(names::MemberPath(member), "plugins/example.dll");
  EXPECT_EQ(names::MemberRelativePath(member), L"plugins\\example.dll");

  for (const char* bad : {"plugins/../x.dll", "plugins/..dll", "plugins\\x.dll", "../nevr-runtime.dll", "/nevr-runtime.dll",
                          "C:nevr-runtime.dll", "plugins/sub/x.dll", "plugins/x.txt", "plugins/.dll", "plugins/CON.dll",
                          "NEVR-RUNTIME.DLL", "nevr-runtime.dll ", "BugSplat64.dll", "plugins/", "", "plugins/x.dll/"}) {
    names::Member parsed;
    EXPECT_FALSE(names::ParseMember(bad, &parsed)) << bad;
  }
}

// ---- files ---------------------------------------------------------------------------------------------------------

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::string Narrow(const std::wstring& text) {
  std::string out;
  for (const wchar_t c : text) out.push_back(static_cast<char>(c));
  return out;
}

// A Unix path as Wine takes it from a Windows API.
std::wstring WinPath(const std::string& unix_path) {
  if (unix_path.empty() || unix_path[0] != '/') return Widen(unix_path);
  std::wstring out = L"Z:";
  for (const char c : unix_path) out.push_back(c == '/' ? L'\\' : static_cast<wchar_t>(c));
  return out;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A handle another program keeps on a file, as a virus scanner or a backup tool can: share mode 0 refuses every other
// open; FILE_SHARE_READ lets the host read the file and refuses its removal.
class Held {
 public:
  Held(const std::wstring& path, DWORD share)
      : handle_(CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
  Held(const Held&) = delete;
  Held& operator=(const Held&) = delete;
  ~Held() { Close(); }
  bool ok() const { return handle_ != INVALID_HANDLE_VALUE; }
  void Close() {
    if (ok()) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_;
};

// `depth` arrays, each holding the next: "[[[]]]" is 3.
std::string Arrays(std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); }

std::string ReadAll(const std::wstring& path) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string text;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) text.append(buffer, read);
  CloseHandle(file);
  return text;
}

void WriteAll(const std::wstring& path, const std::string& bytes) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE) << Narrow(path) << " " << GetLastError();
  DWORD written = 0;
  EXPECT_TRUE(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
  EXPECT_EQ(written, bytes.size());
  CloseHandle(file);
}

nlohmann::json ReadJson(const std::wstring& path) {
  const std::string text = ReadAll(path);
  nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
  return value.is_discarded() ? nlohmann::json() : value;
}

std::string Sha256Hex(const std::string& bytes) {
  HCRYPTPROV provider = 0;
  HCRYPTHASH hash = 0;
  std::string hex;
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return hex;
  if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
      CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0)) {
    BYTE digest[32] = {};
    DWORD length = sizeof(digest);
    if (CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0)) {
      constexpr char kDigits[] = "0123456789abcdef";
      for (const BYTE b : digest) {
        hex.push_back(kDigits[b >> 4]);
        hex.push_back(kDigits[b & 0x0f]);
      }
    }
  }
  if (hash != 0) CryptDestroyHash(hash);
  CryptReleaseContext(provider, 0);
  return hex;
}

void MakeDirs(const std::wstring& path) {
  for (std::size_t at = 3; at <= path.size(); ++at) {
    if (at == path.size() || path[at] == L'\\') CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
  }
}

// Every file under `dir`, relative and sorted, with its bytes: what "unchanged" is compared against.
void Walk(const std::wstring& dir, const std::wstring& prefix, std::map<std::string, std::string>* out) {
  WIN32_FIND_DATAW data;
  const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") continue;
    const std::wstring path = dir + L"\\" + name;
    const std::wstring relative = prefix.empty() ? name : prefix + L"\\" + name;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      (*out)[Narrow(relative) + "\\"] = "";
      Walk(path, relative, out);
    } else {
      (*out)[Narrow(relative)] = ReadAll(path);
    }
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
}

std::map<std::string, std::string> Tree(const std::wstring& dir) {
  std::map<std::string, std::string> out;
  Walk(dir, L"", &out);
  return out;
}

void RemoveTree(const std::wstring& dir) {
  WIN32_FIND_DATAW data;
  const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = data.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring path = dir + L"\\" + name;
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        RemoveDirectoryW(path.c_str());
        DeleteFileW(path.c_str());
      } else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        RemoveTree(path);
      } else {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path.c_str());
      }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
  }
  RemoveDirectoryW(dir.c_str());
}

// ---- records -------------------------------------------------------------------------------------------------------

std::mutex g_records_mutex;
std::vector<nlohmann::json> g_records;

void Sink(const nlohmann::json& record) {
  std::lock_guard<std::mutex> lock(g_records_mutex);
  g_records.push_back(record);
}

std::vector<nlohmann::json> SwapRecords(const std::vector<nlohmann::json>& records) {
  std::vector<nlohmann::json> out;
  for (const nlohmann::json& record : records) {
    if (record.value("event", "") == "update_swap") out.push_back(record);
  }
  return out;
}

std::vector<std::string> Outcomes(const std::vector<nlohmann::json>& records) {
  std::vector<std::string> out;
  for (const nlohmann::json& record : SwapRecords(records)) out.push_back(record.value("outcome", ""));
  return out;
}

// ---- the kill and the failing move ----------------------------------------------------------------------------

std::atomic<int> g_kill_at{0};
std::atomic<int> g_last_step{0};

bool KillAt(int step) {
  g_last_step.store(step);
  return g_kill_at.load() != 0 && step == g_kill_at.load();
}

struct Start {
  UpdateResult result;
  bool lock_held = false;
  std::vector<nlohmann::json> records;
};

const std::map<std::string, std::string> kNoFiles;

class HostUpdateSwap : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* dir = std::getenv("NEVR_SIGFIX_DIR");
    ASSERT_NE(dir, nullptr) << "NEVR_SIGFIX_DIR is not set: run tools/gen_plugin_signature_fixtures.py <dir> --dll-dir <bin>/sigfix "
                               "--plugin-dll <the rendezvous nevr-runtime.dll> and export it (just test-auth-unit does)";
    fixtures_ = dir;
    std::ifstream in(fixtures_ + "/fixtures.json");
    ASSERT_TRUE(in.good()) << "no fixtures.json in " << fixtures_;
    manifest_ = nlohmann::json::parse(in);
    for (const char* key : {"nevr-runtime.signed", "nevr-runtime.unsigned", "good", "good2", "unsigned"}) {
      ASSERT_TRUE(manifest_["files"].contains(key)) << "the fixtures hold no " << key;
    }
    const std::string root_der = ReadAll(WinPath(fixtures_ + "/" + manifest_["root_der"].get<std::string>()));
    ASSERT_FALSE(root_der.empty());
    policy_.rootDer.assign(root_der.begin(), root_der.end());
    policy_.profileEkus = {manifest_["profile_eku"].get<std::string>()};

    wchar_t base[MAX_PATH] = {};
    ASSERT_GT(GetTempPathW(MAX_PATH, base), 0u);
    static LONG counter = 0;
    wchar_t dir_name[MAX_PATH] = {};
    swprintf(dir_name, MAX_PATH, L"%ls\\nevr-update-%lu-%llu-%ld", base, static_cast<unsigned long>(GetCurrentProcessId()),
             static_cast<unsigned long long>(GetTickCount64()), static_cast<long>(InterlockedIncrement(&counter)));
    scratch_ = dir_name;
    ASSERT_NE(CreateDirectoryW(scratch_.c_str(), nullptr), FALSE);
    root_ = scratch_ + L"\\nevr-root";
    local_ = scratch_ + L"\\local";
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    ASSERT_NE(CreateDirectoryW(local_.c_str(), nullptr), FALSE);
    update_ = root_ + L"\\update";
    runtime_path_ = root_ + L"\\nevr-runtime.dll";

    new_runtime_ = Fixture("nevr-runtime.signed");
    old_runtime_ = Fixture("nevr-runtime.unsigned");
    new_plugin_ = Fixture("good");
    old_plugin_ = Fixture("good2");
    ASSERT_FALSE(new_runtime_.empty());
    ASSERT_NE(new_runtime_, old_runtime_);
    ASSERT_NE(new_plugin_, old_plugin_);

    nevr_host::SetVerifyPolicyForTest(&policy_);
    BootstrapDiagnostics::SetEmitSinkForTest(&Sink);
    nevr_host::SetUpdateLockTimingForTest(5000, 20);
    nevr_host::SetUpdateStepHookForTest(&KillAt);
    nevr_host::SetUpdateMoveFailureForTest(0, 0);
    g_kill_at.store(0);
    g_last_step.store(0);
  }

  void TearDown() override {
    nevr_host::SetVerifyPolicyForTest(nullptr);
    BootstrapDiagnostics::SetEmitSinkForTest(nullptr);
    nevr_host::SetUpdateLockTimingForTest(0, 0);
    nevr_host::SetUpdateStepHookForTest(nullptr);
    nevr_host::SetUpdateMoveFailureForTest(0, 0);
    // Only this test's own scratch folder, by its exact path.
    RemoveTree(scratch_);
  }

  std::string Fixture(const char* key) const {
    return ReadAll(WinPath(fixtures_ + "/" + manifest_["files"][key].get<std::string>()));
  }

  void Put(const std::wstring& relative, const std::string& bytes) {
    const std::wstring path = root_ + L"\\" + relative;
    MakeDirs(path.substr(0, path.rfind(L'\\')));
    WriteAll(path, bytes);
  }

  std::string Get(const std::wstring& relative) const { return ReadAll(root_ + L"\\" + relative); }

  // A release the core staged and the runtime published: the files under update\staging\<version>\ and staged.json.
  void Stage(const std::string& version, const std::vector<std::pair<std::string, std::string>>& members) {
    nlohmann::json files = nlohmann::json::array();
    for (const auto& [path, bytes] : members) {
      std::wstring relative = Widen(path);
      std::replace(relative.begin(), relative.end(), L'/', L'\\');
      Put(L"update\\staging\\" + Widen(version) + L"\\" + relative, bytes);
      files.push_back({{"path", path}, {"size", bytes.size()}, {"sha256", Sha256Hex(bytes)}});
    }
    Put(L"update\\staged.json", nlohmann::json({{"version", version}, {"files", files}}).dump());
  }

  void WriteState(const nlohmann::json& state) { Put(L"update\\state.json", state.dump()); }

  // The usual start: the old release in the root (runtime and one owned plugin), the new one staged, and a crash
  // marker an earlier release left, stale once the new release is pending.
  void OldReleaseInTheRootAndANewOneStaged() {
    Put(L"nevr-runtime.dll", old_runtime_);
    Put(L"plugins\\example.dll", old_plugin_);
    WriteState({{"channel", "beta"}, {"owned", {{{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(old_plugin_)}}}}});
    Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}, {"plugins/example.dll", new_plugin_}});
    Put(L"update\\crashed", "");
  }

  // What the root holds after that release was swapped in.
  void ExpectSwapped(int boot_attempts = 1) {
    EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_) << "the staged runtime is in the root";
    EXPECT_EQ(Get(L"plugins\\example.dll"), new_plugin_) << "the staged plugin is in the root";
    EXPECT_EQ(Get(L"update\\previous\\7.7.7\\nevr-runtime.dll"), old_runtime_) << "the old runtime is the previous one";
    EXPECT_EQ(Get(L"update\\previous\\7.7.7\\plugins\\example.dll"), old_plugin_);
    EXPECT_FALSE(Exists(update_ + L"\\staged.json"));
    EXPECT_FALSE(Exists(update_ + L"\\swap.json"));
    EXPECT_FALSE(Exists(update_ + L"\\staging\\1.2.3"));
    EXPECT_FALSE(Exists(update_ + L"\\crashed")) << "the stale crash marker went when the release became pending";
    const nlohmann::json pending = ReadJson(update_ + L"\\pending.json");
    EXPECT_EQ(pending, nlohmann::json({{"to", {{"version", "1.2.3"}, {"sha256", Sha256Hex(new_runtime_)}}},
                                       {"from", {{"version", "7.7.7"}, {"sha256", Sha256Hex(old_runtime_)}}},
                                       {"boot_attempts", boot_attempts},
                                       {"failed_reason", nullptr}}))
        << pending.dump();
    const nlohmann::json state = ReadJson(update_ + L"\\state.json");
    EXPECT_EQ(state.value("channel", ""), "beta") << state.dump();
    EXPECT_EQ(state["owned"], nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(new_runtime_)}},
                                                     {{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(new_plugin_)}}}))
        << state.dump();
  }

  // A root runtime whose NEVR-BUILD field is `version`: the swap reads the first NUL-bounded literal it finds.
  std::string RuntimeNamed(const std::string& version) const {
    std::string bytes(1, '\0');
    bytes += "NEVR-BUILD " + version + " " + std::string(40, 'c');
    bytes.push_back('\0');
    return bytes + old_runtime_;
  }

  // Move the real folder `relative` (to the root) aside and put a directory link where it was. Returns the folder the
  // link points at, or empty (with a failure) when this system cannot make one.
  std::wstring LinkAway(const std::wstring& relative) {
    static LONG counter = 0;
    const std::wstring path = root_ + L"\\" + relative;
    const std::wstring elsewhere = scratch_ + L"\\elsewhere-" + std::to_wstring(InterlockedIncrement(&counter));
    if (MoveFileExW(path.c_str(), elsewhere.c_str(), 0) == FALSE) {
      ADD_FAILURE() << "move " << Narrow(path) << ": " << GetLastError();
      return {};
    }
    if (CreateSymbolicLinkW(path.c_str(), elsewhere.c_str(),
                            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE) {
      ADD_FAILURE() << "link " << Narrow(path) << ": " << GetLastError();
      return {};
    }
    if ((GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      ADD_FAILURE() << "this system does not report the link as a reparse point";
      return {};
    }
    return elsewhere;
  }

  // A fresh root with the usual release staged, a swap of it ended after step `kill`, and the journal's phase then
  // ("" when the kill left no journal).
  std::string KilledSwap(int kill) {
    RemoveTree(root_);
    CreateDirectoryW(root_.c_str(), nullptr);
    OldReleaseInTheRootAndANewOneStaged();
    g_kill_at.store(kill);
    RunStart();
    g_kill_at.store(0);
    const nlohmann::json journal = ReadJson(update_ + L"\\swap.json");
    return journal.is_object() ? journal.value("phase", "") : std::string();
  }

  // The tries the swapped release has had when the next start has run: a start that finds a complete swap (no journal,
  // pending.json there) is another try, and one that finishes an interrupted swap's cleanup is not (that swap's start never
  // got to load).
  int AttemptsAfterTheNextStart() const { return Exists(update_ + L"\\swap.json") || !Exists(update_ + L"\\pending.json") ? 1 : 2; }

  static std::string Journal(const std::string& phase, const std::string& extra = "") {
    const std::string members = "[\"nevr-runtime.dll\", \"plugins/example.dll\"]";
    return "{\"kind\": \"swap\", \"from\": \"7.7.7\", \"to\": \"1.2.3\", \"phase\": \"" + phase + "\", \"out\": " +
           members + ", \"in\": " + members + extra + "}";
  }

  Start RunStart() {
    {
      std::lock_guard<std::mutex> lock(g_records_mutex);
      g_records.clear();
    }
    Start start = RunStartKeepingRecords();
    std::lock_guard<std::mutex> lock(g_records_mutex);
    start.records = g_records;
    return start;
  }

  Start RunStartKeepingRecords() {
    BootstrapDiagnostics diagnostics;
    const std::wstring host_dir = scratch_ + L"\\";
    diagnostics.PrepareForTest(local_.c_str(), host_dir.c_str());
    UpdateLock lock;
    Start start;
    start.result = RunUpdateAtStart(runtime_path_.c_str(), diagnostics, lock);
    start.lock_held = lock.Held();
    return start;
  }

  std::string fixtures_;
  nlohmann::json manifest_;
  nevr_plugin_signature::Policy policy_;
  std::wstring scratch_, root_, local_, update_, runtime_path_;
  std::string new_runtime_, old_runtime_, new_plugin_, old_plugin_;
};

TEST_F(HostUpdateSwap, AStartWithNoUpdateFolderReadsAndWritesNothing) {
  Put(L"nevr-runtime.dll", old_runtime_);
  const auto before = Tree(scratch_);
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kNoUpdateFolder);
  EXPECT_FALSE(start.lock_held);
  EXPECT_TRUE(start.records.empty());
  EXPECT_EQ(Tree(scratch_), before) << "nothing was written anywhere";
}

TEST_F(HostUpdateSwap, AnUpdateFolderWithNothingStagedTakesTheLockAndChangesNothing) {
  Put(L"nevr-runtime.dll", old_runtime_);
  MakeDirs(update_);
  const auto before = Tree(root_);
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.lock_held) << "the lock is held until the runtime is mapped";
  EXPECT_TRUE(SwapRecords(start.records).empty());
  EXPECT_EQ(Tree(root_), before);
}

TEST_F(HostUpdateSwap, AVerifiedStagedReleaseIsSwappedInAndTheOldOneIsKeptAsPrevious) {
  OldReleaseInTheRootAndANewOneStaged();
  Put(L"update\\previous\\0.0.1\\nevr-runtime.dll", "an older generation");
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.lock_held);
  ExpectSwapped();
  EXPECT_FALSE(Exists(update_ + L"\\previous\\0.0.1")) << "one generation is kept";
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "swapped");
  EXPECT_EQ(records[0].value("from", ""), "7.7.7");
  EXPECT_EQ(records[0].value("to", ""), "1.2.3");
  EXPECT_EQ(records[0]["files"],
            nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(new_runtime_)}},
                                   {{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(new_plugin_)}}}));
  EXPECT_TRUE(records[0].contains("elapsed_ms"));
}

TEST_F(HostUpdateSwap, APluginTheNewReleaseNoLongerListsMovesToPreviousAndAStrangersPluginStays) {
  Put(L"nevr-runtime.dll", old_runtime_);
  Put(L"plugins\\dropped.dll", old_plugin_);
  Put(L"plugins\\mine.dll", "a player's own plugin");
  WriteState({{"owned", {{{"path", "plugins/dropped.dll"}, {"sha256", Sha256Hex(old_plugin_)}}}}});
  Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}});
  const Start start = RunStart();
  EXPECT_EQ(Outcomes(start.records), std::vector<std::string>{"swapped"});
  EXPECT_FALSE(Exists(root_ + L"\\plugins\\dropped.dll"));
  EXPECT_EQ(Get(L"update\\previous\\7.7.7\\plugins\\dropped.dll"), old_plugin_);
  EXPECT_EQ(Get(L"plugins\\mine.dll"), "a player's own plugin") << "no stranger's file is moved";
  EXPECT_EQ(ReadJson(update_ + L"\\state.json")["owned"],
            nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(new_runtime_)}}}));
}

// F6: the staged files sat on disk between launches in a folder the player can write.
TEST_F(HostUpdateSwap, F6_AStagedFileChangedOnDiskIsDiscardedCountedAndNothingMoves) {
  OldReleaseInTheRootAndANewOneStaged();
  std::string changed = new_runtime_;
  changed[changed.size() / 2] ^= 0x01;
  Put(L"update\\staging\\1.2.3\\nevr-runtime.dll", changed);
  auto before = Tree(root_);
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "discarded");
  EXPECT_EQ(records[0].value("reason", ""), "sha256");
  EXPECT_FALSE(Exists(update_ + L"\\staged.json")) << "the discarded release is not offered to the next start";
  EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_);
  EXPECT_EQ(Get(L"plugins\\example.dll"), old_plugin_);
  EXPECT_FALSE(Exists(update_ + L"\\pending.json"));
  EXPECT_EQ(ReadJson(update_ + L"\\state.json")["discards"], nlohmann::json({{"1.2.3", 1}}));
  before.erase("update\\staged.json");
  before.erase("update\\state.json");
  auto after = Tree(root_);
  after.erase("update\\state.json");
  EXPECT_EQ(after, before) << "nothing else changed";
}

TEST_F(HostUpdateSwap, F6_AnUnsignedStagedFileIsDiscardedWithTheVerifiersWords) {
  Put(L"nevr-runtime.dll", old_runtime_);
  Stage("1.2.3", {{"nevr-runtime.dll", Fixture("nevr-runtime.unsigned") + std::string("x")}});
  const Start start = RunStart();
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "discarded");
  const std::string reason = records[0].value("reason", "");
  EXPECT_NE(reason.find("unsigned"), std::string::npos) << reason;
  EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_);
}

TEST_F(HostUpdateSwap, F6_AStagedPluginTheVerifierRefusesDiscardsTheWholeRelease) {
  OldReleaseInTheRootAndANewOneStaged();
  Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}, {"plugins/example.dll", Fixture("unsigned")}});
  const Start start = RunStart();
  EXPECT_EQ(Outcomes(start.records), std::vector<std::string>{"discarded"});
  EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_) << "a release is never half applied";
  EXPECT_EQ(Get(L"plugins\\example.dll"), old_plugin_);
}

TEST_F(HostUpdateSwap, F6_TheThirdDiscardQuarantinesTheRelease) {
  OldReleaseInTheRootAndANewOneStaged();
  WriteState({{"channel", "beta"}, {"owned", nlohmann::json::array()}, {"discards", {{"1.2.3", 2}}}});
  const std::string staged_sha = Sha256Hex(new_runtime_);
  std::string changed = new_runtime_;
  changed[100] ^= 0x01;
  Put(L"update\\staging\\1.2.3\\nevr-runtime.dll", changed);
  RunStart();
  const nlohmann::json state = ReadJson(update_ + L"\\state.json");
  ASSERT_TRUE(state["quarantine"].is_array()) << state.dump();
  ASSERT_EQ(state["quarantine"].size(), 1u) << state.dump();
  EXPECT_EQ(state["quarantine"][0].value("version", ""), "1.2.3");
  EXPECT_EQ(state["quarantine"][0].value("runtime_sha256", ""), staged_sha) << "the sha256 the release was staged with";
  EXPECT_EQ(state["quarantine"][0].value("reason", ""), "sha256");
  EXPECT_FALSE(state["quarantine"][0].value("time", "").empty());
  EXPECT_EQ(state.value("channel", ""), "beta") << "the rest of the state is kept";
}

// F7: a kill (or a power loss) after any step of the swap leaves a root the next start makes whole.
TEST_F(HostUpdateSwap, F7_AKillAfterAnyStepIsRecoveredAndTheNextStartEndsWithOneWholeRelease) {
  // Count the steps of an uninterrupted swap.
  OldReleaseInTheRootAndANewOneStaged();
  RunStart();
  const int steps = g_last_step.load();
  ASSERT_GE(steps, 8) << "the swap writes the journal, moves four files and cleans up";
  ExpectSwapped();
  bool saw_recovery_of_a_move = false;
  for (int kill = 1; kill <= steps; ++kill) {
    SCOPED_TRACE(kill);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    OldReleaseInTheRootAndANewOneStaged();
    g_kill_at.store(kill);
    RunStart();
    g_kill_at.store(0);
    // Between the two starts the root holds the old release, the new one, or a journal that says which way to go.
    const bool journal = Exists(update_ + L"\\swap.json");
    const int attempts = AttemptsAfterTheNextStart();
    const Start next = RunStart();
    EXPECT_EQ(next.result.action, UpdateAction::kLoad);
    ExpectSwapped(attempts);
    const auto outcomes = Outcomes(next.records);
    if (journal) {
      ASSERT_FALSE(outcomes.empty());
      EXPECT_EQ(outcomes.front(), "recovered");
      if (outcomes.size() == 2) {
        EXPECT_EQ(outcomes[1], "swapped");
        saw_recovery_of_a_move = true;
      }
    }
  }
  EXPECT_TRUE(saw_recovery_of_a_move) << "some kill fell between two moves and was undone, then swapped again";
}

// F8: a dedicated server beside a client shares the root; the swap waits until nobody runs the runtime. The runtime is
// mapped here by this test's own process: a mapping by a second process is not measured (ADR 0024, "What is not
// known").
TEST_F(HostUpdateSwap, F8_AMappedRuntimeDefersTheSwap) {
  OldReleaseInTheRootAndANewOneStaged();
  const HMODULE mapped = LoadLibraryExW(runtime_path_.c_str(), nullptr, 0);
  ASSERT_NE(mapped, nullptr) << GetLastError();
  const auto before = Tree(root_);
  const Start start = RunStart();
  FreeLibrary(mapped);
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "deferred");
  EXPECT_EQ(records[0].value("reason", ""), "runtime_in_use");
  EXPECT_EQ(records[0].value("win32_error", 0u), static_cast<unsigned>(ERROR_SHARING_VIOLATION));
  EXPECT_EQ(Tree(root_), before) << "staged.json stays for a later start";
}

// F8, review note 5: only a sharing violation says the runtime is in use. A write-open refused for another reason (here
// a read-only file: access denied) blocks the swap under its own reason and error, never as "in use".
TEST_F(HostUpdateSwap, F8_ARuntimeThatRefusesAWriteOpenForAnotherReasonBlocksWithThatError) {
  OldReleaseInTheRootAndANewOneStaged();
  ASSERT_NE(SetFileAttributesW(runtime_path_.c_str(), FILE_ATTRIBUTE_READONLY), FALSE) << GetLastError();
  const auto before = Tree(root_);
  const Start start = RunStart();
  SetFileAttributesW(runtime_path_.c_str(), FILE_ATTRIBUTE_NORMAL);
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "blocked");
  EXPECT_EQ(records[0].value("reason", ""), "runtime_not_writable");
  EXPECT_EQ(records[0].value("win32_error", 0u), static_cast<unsigned>(ERROR_ACCESS_DENIED));
  EXPECT_EQ(Tree(root_), before) << "nothing moved; staged.json stays";
}

// F23: a host that cannot get the lock in time.
TEST_F(HostUpdateSwap, F23_ALockHeldElsewhereTimesOutAndWithNoJournalLoadsWithoutTouchingTheUpdateFolder) {
  OldReleaseInTheRootAndANewOneStaged();
  nevr_host::SetUpdateLockTimingForTest(300, 25);
  UpdateLock other;
  ASSERT_TRUE(other.Acquire(update_.c_str(), 1000, 25));
  const auto before = Tree(root_);
  const ULONGLONG started = GetTickCount64();
  const Start start = RunStart();
  EXPECT_GE(GetTickCount64() - started, 250u) << "it waited";
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_FALSE(start.lock_held);
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "blocked");
  EXPECT_EQ(records[0].value("reason", ""), "lock_timeout");
  EXPECT_EQ(Tree(root_), before);
}

TEST_F(HostUpdateSwap, F23_ALockHeldElsewhereWithAJournalStopsTheStart) {
  OldReleaseInTheRootAndANewOneStaged();
  Put(L"update\\swap.json", nlohmann::json({{"kind", "swap"}, {"from", "7.7.7"}, {"to", "1.2.3"}, {"phase", "out"},
                                            {"out", {"nevr-runtime.dll"}}, {"in", {"nevr-runtime.dll"}}})
                                .dump());
  nevr_host::SetUpdateLockTimingForTest(200, 25);
  UpdateLock other;
  ASSERT_TRUE(other.Acquire(update_.c_str(), 1000, 25));
  const auto before = Tree(root_);
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kStop);
  ASSERT_NE(start.result.reason, nullptr);
  EXPECT_STREQ(start.result.reason, "lock_timeout");
  EXPECT_EQ(Tree(root_), before);
}

// F17: two game processes start together: the second waits, then finds the work done.
TEST_F(HostUpdateSwap, F17_TwoStartsAtOnceSwapOnceAndTheSecondFindsTheWorkDone) {
  OldReleaseInTheRootAndANewOneStaged();
  {
    std::lock_guard<std::mutex> lock(g_records_mutex);
    g_records.clear();
  }
  std::vector<std::thread> starts;
  std::vector<Start> results(2);
  for (int i = 0; i < 2; ++i) {
    starts.emplace_back([this, &results, i] { results[i] = RunStartKeepingRecords(); });
  }
  for (std::thread& start : starts) start.join();
  std::vector<nlohmann::json> records;
  {
    std::lock_guard<std::mutex> lock(g_records_mutex);
    records = g_records;
  }
  EXPECT_EQ(Outcomes(records), std::vector<std::string>{"swapped"}) << "one start swapped; the other found nothing to do";
  EXPECT_EQ(results[0].result.action, UpdateAction::kLoad);
  EXPECT_EQ(results[1].result.action, UpdateAction::kLoad);
  // The first start's runtime is not mapped in this test, so the second start that finds the swap done is another try.
  ExpectSwapped(2);
}

// ADR 0024, section 4.1: the lock is held from the first read of update\ until the runtime is mapped, and released
// before its initialize, so a second host cannot move the runtime between the first host's probe and its load.
std::wstring g_update_dir;
int g_free_before_load = -1;
int g_free_after_map = -1;

int LockIsFree() {
  UpdateLock probe;
  return probe.Acquire(g_update_dir.c_str(), 0, 1) ? 1 : 0;
}
void BeforeLoad(const wchar_t*) { g_free_before_load = LockIsFree(); }
void AfterMap(const wchar_t*) { g_free_after_map = LockIsFree(); }

TEST_F(HostUpdateSwap, F17_TheLoaderHoldsTheLockUntilTheRuntimeIsMappedAndReleasesItBeforeInitialize) {
  Put(L"nevr-runtime.dll", new_runtime_);
  MakeDirs(update_);
  g_update_dir = update_;
  g_free_before_load = -1;
  g_free_after_map = -1;
  nevr_host::SetRuntimeDirForTest(root_.c_str());
  nevr_host::SetVerifyRuntimeForTest(true);
  nevr_host::SetDevBuildForTest(0);
  nevr_host::SetBeforeLoadHookForTest(&BeforeLoad);
  nevr_host::SetAfterMapHookForTest(&AfterMap);
  UpdateLock lock;
  ASSERT_TRUE(lock.Acquire(update_.c_str(), 1000, 25));
  BootstrapDiagnostics diagnostics;
  const std::wstring host_dir = scratch_ + L"\\";
  diagnostics.PrepareForTest(local_.c_str(), host_dir.c_str());
  nevr_host::RuntimeLoader loader;
  loader.Prepare();
  NevrHostContext context{};  // the stand-in's initialize refuses it: the lock is what this test is about
  const nevr_host::RuntimeLoadResult result = loader.Load(context, diagnostics, &lock);
  nevr_host::SetRuntimeDirForTest(nullptr);
  nevr_host::SetVerifyRuntimeForTest(false);
  nevr_host::SetDevBuildForTest(-1);
  nevr_host::SetBeforeLoadHookForTest(nullptr);
  nevr_host::SetAfterMapHookForTest(nullptr);
  if (result.module != nullptr) FreeLibrary(result.module);
  EXPECT_NE(result.module, nullptr) << "the signed stand-in was mapped";
  EXPECT_EQ(g_free_before_load, 0) << "held while LoadLibraryExW runs";
  EXPECT_EQ(g_free_after_map, 1) << "released once the runtime is mapped, before initialize";
  EXPECT_FALSE(lock.Held());
}

TEST_F(HostUpdateSwap, F17_ARefusedRuntimeReleasesTheLockToo) {
  Put(L"nevr-runtime.dll", old_runtime_);  // unsigned: refused, never mapped
  MakeDirs(update_);
  nevr_host::SetRuntimeDirForTest(root_.c_str());
  nevr_host::SetVerifyRuntimeForTest(true);
  nevr_host::SetDevBuildForTest(0);
  UpdateLock lock;
  ASSERT_TRUE(lock.Acquire(update_.c_str(), 1000, 25));
  BootstrapDiagnostics diagnostics;
  const std::wstring host_dir = scratch_ + L"\\";
  diagnostics.PrepareForTest(local_.c_str(), host_dir.c_str());
  nevr_host::RuntimeLoader loader;
  loader.Prepare();
  NevrHostContext context{};
  const nevr_host::RuntimeLoadResult result = loader.Load(context, diagnostics, &lock);
  nevr_host::SetRuntimeDirForTest(nullptr);
  nevr_host::SetVerifyRuntimeForTest(false);
  nevr_host::SetDevBuildForTest(-1);
  EXPECT_EQ(result.outcome, nevr_host::RuntimeLoadOutcome::kRuntimeRefused);
  EXPECT_FALSE(lock.Held());
}

// F18: a file self-update did not put there is never replaced.
TEST_F(HostUpdateSwap, F18_AForeignFileWhereAStagedFileGoesBlocksTheWholeSwap) {
  for (const bool listed_with_other_bytes : {false, true}) {
    SCOPED_TRACE(listed_with_other_bytes);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    Put(L"nevr-runtime.dll", old_runtime_);
    Put(L"plugins\\example.dll", "a plugin the player put here");
    WriteState({{"owned", listed_with_other_bytes
                              ? nlohmann::json::array({{{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(old_plugin_)}}})
                              : nlohmann::json::array()}});
    Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}, {"plugins/example.dll", new_plugin_}});
    const auto before = Tree(root_);
    const Start start = RunStart();
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "blocked");
    EXPECT_EQ(records[0].value("reason", ""), "foreign_file");
    EXPECT_EQ(Tree(root_), before) << "nothing moved, nothing discarded";
  }
}

// F22: a name outside the vocabulary is never acted on. Each bad staged.json is paired with a file at the place a
// careless join would reach, so a swap that followed the name would be seen.
TEST_F(HostUpdateSwap, F22_AStagedJsonOutsideTheVocabularyIsNotUsedAndNothingOutsideTheFoldersIsTouched) {
  struct Case {
    const char* label;
    std::string staged;
  };
  const std::string runtime_sha = Sha256Hex(new_runtime_);
  const std::string size = std::to_string(new_runtime_.size());
  auto staged = [&](const std::string& version, const std::string& path) {
    return nlohmann::json({{"version", version},
                           {"files", {{{"path", path}, {"size", new_runtime_.size()}, {"sha256", runtime_sha}}}}})
        .dump();
  };
  // A good runtime beside one bad name: the bad name alone must make the file unusable, not the missing runtime.
  auto beside_runtime = [&](const std::string& path) {
    return nlohmann::json({{"version", "1.2.3"},
                           {"files", nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"size", new_runtime_.size()}, {"sha256", runtime_sha}},
                                                            {{"path", path}, {"size", new_plugin_.size()}, {"sha256", Sha256Hex(new_plugin_)}}})}})
        .dump();
  };
  const std::vector<Case> cases = {
      {"version ..", staged("..", "nevr-runtime.dll")},
      {"version CON", staged("CON", "nevr-runtime.dll")},
      {"version with a backslash", staged("a\\b", "nevr-runtime.dll")},
      {"path ../", staged("1.2.3", "../nevr-runtime.dll")},
      {"path plugins/../", staged("1.2.3", "plugins/../nevr-runtime.dll")},
      {"path with a backslash", staged("1.2.3", "plugins\\x.dll")},
      {"device plugin", staged("1.2.3", "plugins/CON.dll")},
      {"no runtime", staged("1.2.3", "plugins/example.dll")},
      {"runtime and plugins/../", beside_runtime("plugins/../example.dll")},
      {"runtime and a backslash", beside_runtime("plugins\\example.dll")},
      {"runtime and a device plugin", beside_runtime("plugins/CON.dll")},
      {"runtime and a plugin named .", beside_runtime("plugins/..dll")},
      {"the runtime twice", beside_runtime("nevr-runtime.dll")},
      // Review note 10: Windows names are one file in any case, so two spellings of one plugin are one file twice.
      {"one plugin twice, in two cases",
       nlohmann::json({{"version", "1.2.3"},
                       {"files", nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"size", new_runtime_.size()}, {"sha256", runtime_sha}},
                                                        {{"path", "plugins/Example.dll"}, {"size", new_plugin_.size()}, {"sha256", Sha256Hex(new_plugin_)}},
                                                        {{"path", "plugins/example.dll"}, {"size", new_plugin_.size()}, {"sha256", Sha256Hex(new_plugin_)}}})}})
           .dump()},
      {"not JSON", "{\"version\": \"1.2.3\", \"files\": ["},
      {"too large", "{\"version\": \"1.2.3\", \"pad\": \"" + std::string(70 * 1024, 'x') + "\"}"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.label);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    Put(L"nevr-runtime.dll", old_runtime_);
    // Where a careless join of each name would read from.
    Put(L"update\\nevr-runtime.dll", new_runtime_);
    Put(L"update\\staging\\nevr-runtime.dll", new_runtime_);
    Put(L"update\\staging\\1.2.3\\nevr-runtime.dll", new_runtime_);
    Put(L"update\\staging\\1.2.3\\plugins\\example.dll", new_plugin_);
    Put(L"update\\staged.json", c.staged);
    auto before = Tree(root_);
    const Start start = RunStart();
    EXPECT_EQ(start.result.action, UpdateAction::kLoad);
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "discarded");
    EXPECT_EQ(records[0].value("reason", ""), "bad_staged");
    EXPECT_FALSE(Exists(update_ + L"\\staged.json"));
    before.erase("update\\staged.json");
    EXPECT_EQ(Tree(root_), before) << "nothing else changed, and nothing was counted";
  }
}

TEST_F(HostUpdateSwap, F22_AJournalThisHostCannotReadStopsTheStartAndIsNotExecuted) {
  const std::vector<std::string> journals = {
      "{\"kind\": \"swap\", ",
      nlohmann::json({{"kind", "something"}, {"from", "7.7.7"}, {"to", "1.2.3"}, {"phase", "out"}, {"out", nlohmann::json::array()}, {"in", nlohmann::json::array()}}).dump(),
      nlohmann::json({{"kind", "swap"}, {"from", "7.7.7"}, {"to", ".."}, {"phase", "in"}, {"out", {"nevr-runtime.dll"}}, {"in", {"nevr-runtime.dll"}}}).dump(),
      nlohmann::json({{"kind", "swap"}, {"from", "7.7.7"}, {"to", "1.2.3"}, {"phase", "in"}, {"out", {"nevr-runtime.dll"}}, {"in", {"../x.dll"}}}).dump(),
      nlohmann::json({{"kind", "swap"}, {"from", "7.7.7"}, {"to", "1.2.3"}, {"phase", "sideways"}, {"out", {"nevr-runtime.dll"}}, {"in", {"nevr-runtime.dll"}}}).dump(),
  };
  for (const std::string& journal : journals) {
    SCOPED_TRACE(journal.substr(0, 60));
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    OldReleaseInTheRootAndANewOneStaged();
    Put(L"update\\swap.json", journal);
    const auto before = Tree(root_);
    const Start start = RunStart();
    EXPECT_EQ(start.result.action, UpdateAction::kStop);
    ASSERT_NE(start.result.reason, nullptr);
    EXPECT_STREQ(start.result.reason, "bad_journal");
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "blocked");
    EXPECT_EQ(records[0].value("reason", ""), "bad_journal");
    EXPECT_EQ(Tree(root_), before);
  }
}

// Every folder the swap reads, moves into or deletes from, as a link out of the root: nothing behind the link is
// moved or deleted, and the record names the folder (logging.md Rule 1). update\previous\<v>\ is the older generation
// the swap clears; staging\<v>\ is the staged release's.
TEST_F(HostUpdateSwap, F22_AReparsePointInTheUpdateFoldersBlocksTheSwap) {
  struct Case {
    const wchar_t* linked;
    const char* folder;
  };
  for (const Case& c : {Case{L"update", "update"}, Case{L"update\\staging", "update/staging"},
                        Case{L"update\\previous", "update/previous"}, Case{L"plugins", "plugins"},
                        Case{L"update\\staging\\1.2.3", "update/staging/1.2.3"},
                        Case{L"update\\staging\\1.2.3\\plugins", "update/staging/1.2.3/plugins"},
                        Case{L"update\\previous\\0.0.1", "update/previous/0.0.1"},
                        Case{L"update\\previous\\0.0.1\\plugins", "update/previous/0.0.1/plugins"}}) {
    SCOPED_TRACE(c.folder);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    OldReleaseInTheRootAndANewOneStaged();
    Put(L"update\\previous\\0.0.1\\nevr-runtime.dll", "an older generation");
    Put(L"update\\previous\\0.0.1\\plugins\\older.dll", "an older generation's plugin");
    const std::wstring elsewhere = LinkAway(c.linked);
    ASSERT_FALSE(elsewhere.empty());
    const auto outside = Tree(elsewhere);
    const std::string before_runtime = Get(L"nevr-runtime.dll");
    const Start start = RunStart();
    EXPECT_EQ(start.result.action, UpdateAction::kLoad);
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "blocked");
    EXPECT_EQ(records[0].value("reason", ""), "reparse_point");
    EXPECT_EQ(records[0].value("folder", ""), c.folder);
    EXPECT_EQ(Get(L"nevr-runtime.dll"), before_runtime);
    EXPECT_TRUE(Exists(update_ + L"\\staged.json"));
    EXPECT_EQ(Tree(elsewhere), outside) << "nothing behind the link was moved, made or deleted";
  }
}

// update\lock is a file inside update\: a link there is refused before the lock is taken through it. Here a lock is
// held in the link's target, so a host that took the lock first would wait for it and time out.
TEST_F(HostUpdateSwap, F22_ALinkedUpdateFolderIsRefusedBeforeTheLockIsTakenThroughIt) {
  OldReleaseInTheRootAndANewOneStaged();
  const std::wstring elsewhere = LinkAway(L"update");
  ASSERT_FALSE(elsewhere.empty());
  nevr_host::SetUpdateLockTimingForTest(300, 25);
  UpdateLock other;
  ASSERT_TRUE(other.Acquire(elsewhere.c_str(), 1000, 25));
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_FALSE(start.lock_held);
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("reason", ""), "reparse_point");
  EXPECT_EQ(records[0].value("folder", ""), "update");
}

// A link beside a journal: the root may be half moved and recovery would move files through the link, so the start
// stops (ADR 0024, section 4.1's rule for a journal the host cannot act on).
TEST_F(HostUpdateSwap, F22_AReparsePointBesideAJournalStopsTheStart) {
  OldReleaseInTheRootAndANewOneStaged();
  Put(L"update\\swap.json", Journal("out"));
  ASSERT_FALSE(LinkAway(L"update\\staging").empty());
  const auto before = Tree(root_);
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kStop);
  ASSERT_NE(start.result.reason, nullptr);
  EXPECT_STREQ(start.result.reason, "reparse_point");
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("folder", ""), "update/staging");
  EXPECT_EQ(records[0].value("journal", false), true);
  EXPECT_EQ(Tree(root_), before);
}

// Recovery moves files between the root and previous\<from>\ or staging\<to>\: with either one a link, it moves
// nothing. In phase out or in the root may be half moved, so the start stops; in phase done every file is in place and
// the start loads, leaving the cleanup and the journal.
TEST_F(HostUpdateSwap, F22_ARecoveryNeverMovesFilesThroughALinkedFolder) {
  ASSERT_EQ(KilledSwap(5), "in");
  std::wstring elsewhere = LinkAway(L"update\\previous\\7.7.7");
  ASSERT_FALSE(elsewhere.empty());
  auto outside = Tree(elsewhere);
  ASSERT_EQ(outside.count("nevr-runtime.dll"), 1u) << "the moved-out runtime is behind the link";
  Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kStop);
  ASSERT_NE(start.result.reason, nullptr);
  EXPECT_STREQ(start.result.reason, "reparse_point");
  auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("folder", ""), "update/previous/7.7.7");
  EXPECT_EQ(records[0].value("phase", ""), "in");
  EXPECT_EQ(Tree(elsewhere), outside) << "nothing was moved back through the link";
  EXPECT_TRUE(Exists(update_ + L"\\swap.json"));

  ASSERT_EQ(KilledSwap(7), "done");
  elsewhere = LinkAway(L"update\\staging\\1.2.3");
  ASSERT_FALSE(elsewhere.empty());
  outside = Tree(elsewhere);
  ASSERT_EQ(outside.count("plugins\\"), 1u) << "the emptied staging folder's plugins folder is behind the link";
  start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("reason", ""), "reparse_point");
  EXPECT_EQ(records[0].value("folder", ""), "update/staging/1.2.3");
  EXPECT_EQ(Tree(elsewhere), outside) << "no folder behind the link was removed";
  EXPECT_TRUE(Exists(update_ + L"\\swap.json"));
}

// The review's probe 4: an older generation in previous\ that is a link out of the root. Clearing it deleted the files
// behind the link; it must leave them, whether the version folder or its plugins folder is the link.
TEST_F(HostUpdateSwap, F22_ALinkedOlderGenerationIsNotClearedThroughTheLink) {
  for (const wchar_t* linked : {L"update\\previous\\0.0.1", L"update\\previous\\0.0.1\\plugins"}) {
    SCOPED_TRACE(Narrow(linked));
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    OldReleaseInTheRootAndANewOneStaged();
    Put(L"update\\previous\\0.0.1\\nevr-runtime.dll", "a file outside the root");
    Put(L"update\\previous\\0.0.1\\plugins\\anything.dll", "a plugin outside the root");
    const std::wstring victim = LinkAway(linked);
    ASSERT_FALSE(victim.empty());
    const auto outside = Tree(victim);
    ASSERT_FALSE(outside.empty());
    const auto before = Tree(root_);
    const Start start = RunStart();
    EXPECT_EQ(start.result.action, UpdateAction::kLoad);
    EXPECT_EQ(Tree(victim), outside) << "a file outside the nevr root was deleted";
    EXPECT_EQ(Tree(root_), before) << "nothing moved; staged.json stays";
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("reason", ""), "reparse_point");
  }
}

// F22: a move that fails (to another volume: MoveFileExW without MOVEFILE_COPY_ALLOWED) is undone by the journal.
TEST_F(HostUpdateSwap, F22_AMoveThatFailsIsUndoneAndTheRootIsAsItWas) {
  for (int failing = 1; failing <= 4; ++failing) {
    SCOPED_TRACE(failing);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    OldReleaseInTheRootAndANewOneStaged();
    auto before = Tree(root_);
    nevr_host::SetUpdateMoveFailureForTest(failing, ERROR_NOT_SAME_DEVICE);
    const Start start = RunStart();
    nevr_host::SetUpdateMoveFailureForTest(0, 0);
    EXPECT_EQ(start.result.action, UpdateAction::kLoad);
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "blocked");
    EXPECT_EQ(records[0].value("reason", ""), "move_failed");
    EXPECT_EQ(records[0].value("win32_error", 0u), static_cast<unsigned>(ERROR_NOT_SAME_DEVICE));
    auto after = Tree(root_);
    // The empty folders the swap made for previous\ may stay; every file is back where it was.
    for (auto it = after.begin(); it != after.end();) {
      it = (it->first.back() == '\\' && before.count(it->first) == 0) ? after.erase(it) : std::next(it);
    }
    EXPECT_EQ(after, before);
    const Start retry = RunStart();
    EXPECT_EQ(Outcomes(retry.records), std::vector<std::string>{"swapped"}) << "the next start swaps";
    ExpectSwapped();
  }
}

// ADR 0024, section 4.8: a state.json that does not parse is rebuilt from the runtime in the root, and the host says so.
TEST_F(HostUpdateSwap, ATornStateJsonIsRebuiltWithOnlyTheRuntimeOwned) {
  Put(L"nevr-runtime.dll", old_runtime_);
  Put(L"plugins\\example.dll", old_plugin_);
  Put(L"update\\state.json", "{\"owned\": [");
  Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}});
  const Start start = RunStart();
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].value("outcome", ""), "recovered");
  EXPECT_EQ(records[0].value("reason", ""), "state_rebuilt");
  EXPECT_EQ(records[1].value("outcome", ""), "swapped");
  EXPECT_EQ(Get(L"plugins\\example.dll"), old_plugin_) << "a plugin the rebuilt state does not list is not ours to move";
  const nlohmann::json state = ReadJson(update_ + L"\\state.json");
  EXPECT_EQ(state["owned"], nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(new_runtime_)}}}))
      << state.dump();
}

// ---- review fixes (#2108) ------------------------------------------------------------------------------------------

// B1 (ADR 0024 section 4.2; #2104's falsifier "a path or version outside the vocabulary is acted on"): the root runtime
// is moved out before anything verifies it, so the version in its NEVR-BUILD field is a name read from a file. One
// outside the vocabulary blocks the swap: it never names update\previous\<from>\ and is never logged. The review's
// probes: "..\..\..\escaped" moved the runtime out of the nevr root; "9.9.9_x" (a legal file name) left a journal the
// host then refused at every start. Each runs once with a kill after the second step, then once to its end.
TEST_F(HostUpdateSwap, B1_ARootRuntimeVersionOutsideTheVocabularyBlocksTheSwapAndNeverBecomesAPath) {
  for (const std::string& version : {std::string("..\\..\\..\\escaped"), std::string("9.9.9_x"), std::string("CON"),
                                     std::string("a/b"), std::string("\xff\xfe")}) {
    SCOPED_TRACE(version);
    RemoveTree(root_);
    RemoveTree(scratch_ + L"\\escaped");
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    OldReleaseInTheRootAndANewOneStaged();
    Put(L"nevr-runtime.dll", RuntimeNamed(version));
    const auto before = Tree(root_);
    for (const int kill : {2, 0}) {
      SCOPED_TRACE(kill);
      g_kill_at.store(kill);
      const Start start = RunStart();
      g_kill_at.store(0);
      EXPECT_FALSE(Exists(scratch_ + L"\\escaped\\nevr-runtime.dll")) << "the runtime was moved out of the nevr root";
      EXPECT_EQ(Tree(root_), before) << "nothing moved, no journal, staged.json kept";
      EXPECT_EQ(start.result.action, UpdateAction::kLoad);
      const auto records = SwapRecords(start.records);
      ASSERT_EQ(records.size(), 1u);
      EXPECT_EQ(records[0].value("outcome", ""), "blocked");
      EXPECT_EQ(records[0].value("reason", ""), "bad_from_version");
      EXPECT_EQ(records[0].value("to", ""), "1.2.3");
      EXPECT_FALSE(records[0].contains("from")) << "a name outside the vocabulary is not logged";
    }
  }
}

// B1's sibling: a state.json rebuilt from the root runtime (section 4.8) records that runtime as `current` only when its
// version is a version name.
TEST_F(HostUpdateSwap, B1_ARebuiltStateNamesNoCurrentReleaseWhoseVersionIsOutsideTheVocabulary) {
  for (const std::string& version : {std::string("..\\x"), std::string("\xff")}) {
    SCOPED_TRACE(version);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    const std::string runtime = RuntimeNamed(version);
    Put(L"nevr-runtime.dll", runtime);
    Put(L"update\\state.json", "{\"owned\": [");
    Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}});
    const Start start = RunStart();
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].value("reason", ""), "state_rebuilt");
    EXPECT_EQ(records[0].value("state_written", false), true);
    EXPECT_EQ(records[1].value("reason", ""), "bad_from_version");
    const nlohmann::json state = ReadJson(update_ + L"\\state.json");
    EXPECT_FALSE(state.contains("current")) << state.dump();
    EXPECT_EQ(state["owned"], nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(runtime)}}}));
  }
}

// B2 (logging.md Rule 10; ADR 0024 section 4.6): a file the host cannot open is not a damaged file. The review's
// probe 3: a valid journal another program held open read as bad_journal, with no error code.
TEST_F(HostUpdateSwap, B2_AJournalAnotherProgramHoldsOpenStopsUnderItsOwnReasonWithTheError) {
  OldReleaseInTheRootAndANewOneStaged();
  Put(L"update\\swap.json", Journal("out"));
  {
    Held held(update_ + L"\\swap.json", 0);
    ASSERT_TRUE(held.ok());
    const auto before = Tree(root_);
    const Start start = RunStart();
    EXPECT_EQ(start.result.action, UpdateAction::kStop);
    ASSERT_NE(start.result.reason, nullptr);
    EXPECT_STREQ(start.result.reason, "journal_unreadable");
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "blocked");
    EXPECT_EQ(records[0].value("reason", ""), "journal_unreadable");
    EXPECT_EQ(records[0].value("win32_error", 0u), static_cast<unsigned>(ERROR_SHARING_VIOLATION));
    EXPECT_EQ(Tree(root_), before);
  }
  const Start next = RunStart();
  EXPECT_EQ(Outcomes(next.records), (std::vector<std::string>{"recovered", "swapped"})) << "the journal was fine";
  ExpectSwapped();
}

TEST_F(HostUpdateSwap, B2_AStagedJsonAnotherProgramHoldsOpenIsLeftWhereItIs) {
  OldReleaseInTheRootAndANewOneStaged();
  const std::string staged = Get(L"update\\staged.json");
  {
    Held held(update_ + L"\\staged.json", 0);
    ASSERT_TRUE(held.ok());
    const auto before = Tree(root_);
    const Start start = RunStart();
    EXPECT_EQ(start.result.action, UpdateAction::kLoad);
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "deferred");
    EXPECT_EQ(records[0].value("reason", ""), "staged_unreadable");
    EXPECT_EQ(records[0].value("win32_error", 0u), static_cast<unsigned>(ERROR_SHARING_VIOLATION));
    EXPECT_EQ(Tree(root_), before);
  }
  EXPECT_EQ(Get(L"update\\staged.json"), staged) << "not deleted, not counted";
  EXPECT_FALSE(ReadJson(update_ + L"\\state.json").contains("discards"));
  EXPECT_EQ(Outcomes(RunStart().records), std::vector<std::string>{"swapped"});
}

TEST_F(HostUpdateSwap, B2_AStateJsonAnotherProgramHoldsOpenIsNeitherRebuiltNorSwappedWithout) {
  OldReleaseInTheRootAndANewOneStaged();
  WriteState({{"channel", "beta"},
              {"owned", {{{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(old_plugin_)}}}},
              {"discards", {{"0.9.0", 1}}},
              {"quarantine", nlohmann::json::array({{{"version", "0.8.0"}}})}});
  const std::string state = Get(L"update\\state.json");
  {
    Held held(update_ + L"\\state.json", 0);
    ASSERT_TRUE(held.ok());
    const auto before = Tree(root_);
    const Start start = RunStart();
    EXPECT_EQ(start.result.action, UpdateAction::kLoad);
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "deferred");
    EXPECT_EQ(records[0].value("reason", ""), "state_unreadable");
    EXPECT_EQ(records[0].value("win32_error", 0u), static_cast<unsigned>(ERROR_SHARING_VIOLATION));
    EXPECT_EQ(Tree(root_), before) << "no swap without the ownership list";
  }
  EXPECT_EQ(Get(L"update\\state.json"), state) << "channel, discards and quarantine are kept";
  EXPECT_EQ(Outcomes(RunStart().records), std::vector<std::string>{"swapped"});
}

// An interrupted swap does not need state.json to be undone, so it is undone; phase done's cleanup writes state.json,
// so it waits for a start that can read it. state.json is never overwritten while it cannot be read.
TEST_F(HostUpdateSwap, B2_AnInterruptedSwapIsStillUndoneWhileStateJsonCannotBeReadAndItsCleanupWaits) {
  for (const int kill : {5, 7}) {
    SCOPED_TRACE(kill);
    const std::string phase = KilledSwap(kill);
    ASSERT_EQ(phase, kill == 5 ? "in" : "done");
    const std::string state = Get(L"update\\state.json");
    {
      Held held(update_ + L"\\state.json", 0);
      ASSERT_TRUE(held.ok());
      const Start start = RunStart();
      EXPECT_EQ(start.result.action, UpdateAction::kLoad);
      if (phase == "in") {
        EXPECT_EQ(Outcomes(start.records), (std::vector<std::string>{"deferred", "recovered"}));
        EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_) << "undone to the old release";
        EXPECT_EQ(Get(L"plugins\\example.dll"), old_plugin_);
        EXPECT_FALSE(Exists(update_ + L"\\swap.json"));
        EXPECT_TRUE(Exists(update_ + L"\\staged.json")) << "no new swap without state.json";
      } else {
        EXPECT_EQ(Outcomes(start.records), std::vector<std::string>{"deferred"});
        EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_) << "every file is in place";
        EXPECT_TRUE(Exists(update_ + L"\\swap.json")) << "the cleanup waits";
        EXPECT_FALSE(Exists(update_ + L"\\pending.json"));
      }
    }
    EXPECT_EQ(Get(L"update\\state.json"), state);
    RunStart();
    ExpectSwapped();
  }
}

// Review note 3: a state.json nested 32,000 arrays deep in 64 KiB ended the host. The text is measured before it is
// parsed; a file nested past names::kMaxJsonDepth is damaged, whichever file it is, and one at the cap is not.
TEST_F(HostUpdateSwap, AJsonFileNestedPastTheCapIsDamagedAndOneAtTheCapIsNot) {
  const std::size_t cap = names::kMaxJsonDepth;
  const std::string runtime_sha = Sha256Hex(new_runtime_);
  for (const std::size_t depth : {cap, cap + 1, std::size_t{32000}}) {
    SCOPED_TRACE(depth);
    const bool deep = depth > cap;
    // state.json: one object holding `depth - 1` arrays.
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    Put(L"nevr-runtime.dll", old_runtime_);
    Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}});
    Put(L"update\\state.json", "{\"channel\": \"beta\", \"x\": " + Arrays(depth - 1) + "}");
    Start start = RunStart();
    EXPECT_EQ(Outcomes(start.records),
              deep ? (std::vector<std::string>{"recovered", "swapped"}) : std::vector<std::string>{"swapped"});
    EXPECT_EQ(ReadJson(update_ + L"\\state.json").value("channel", ""), deep ? "" : "beta");
    if (depth > cap + 1) continue;  // staged.json and swap.json at 32,000 are the same reader
    // staged.json: valid, plus a key holding `depth - 1` arrays.
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    Put(L"nevr-runtime.dll", old_runtime_);
    Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}});
    Put(L"update\\staged.json",
        "{\"version\": \"1.2.3\", \"files\": [{\"path\": \"nevr-runtime.dll\", \"size\": " +
            std::to_string(new_runtime_.size()) + ", \"sha256\": \"" + runtime_sha + "\"}], \"x\": " + Arrays(depth - 1) +
            "}");
    start = RunStart();
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("reason", ""), deep ? "bad_staged" : "");
    // swap.json: a phase-out journal, plus a key holding `depth - 1` arrays.
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    OldReleaseInTheRootAndANewOneStaged();
    Put(L"update\\swap.json", Journal("out", ", \"x\": " + Arrays(depth - 1)));
    start = RunStart();
    EXPECT_EQ(start.result.action, deep ? UpdateAction::kStop : UpdateAction::kLoad);
    EXPECT_EQ(SwapRecords(start.records).at(0).value("reason", ""), deep ? "bad_journal" : "");
  }
}

// Review note 2, the review's probe 5: a done journal a start could not finish is finished by a later one. By then the
// runtime may have published a newer staged.json, and the pending record and a crash marker may say how the swapped
// release did since (#2105 reads them). The cleanup takes none of them.
TEST_F(HostUpdateSwap, F7_ALeftoverDoneJournalKeepsANewerStagedReleaseForTheSwapThatFollows) {
  OldReleaseInTheRootAndANewOneStaged();
  ASSERT_EQ(Outcomes(RunStart().records), std::vector<std::string>{"swapped"});
  Put(L"update\\swap.json", Journal("done"));
  Stage("2.0.0", {{"nevr-runtime.dll", new_runtime_}, {"plugins/example.dll", new_plugin_}});
  const Start start = RunStart();
  // The cleanup keeps the newer staged.json. The first release is still pending, so the newer one waits for it to be
  // confirmed (#2105, ADR 0024 section 6): swapping now would move the unconfirmed release over the last confirmed one.
  EXPECT_EQ(Outcomes(start.records), (std::vector<std::string>{"recovered", "deferred"}));
  EXPECT_TRUE(Exists(update_ + L"\\staged.json"));
  EXPECT_EQ(ReadJson(update_ + L"\\pending.json")["to"].value("version", ""), "1.2.3");
  DeleteFileW((update_ + L"\\pending.json").c_str());  // the runtime's UpdateConfirm
  const Start next = RunStart();
  EXPECT_EQ(Outcomes(next.records), std::vector<std::string>{"swapped"});
  const auto records = SwapRecords(next.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("to", ""), "2.0.0");
  EXPECT_EQ(ReadJson(update_ + L"\\pending.json")["to"].value("version", ""), "2.0.0");
  EXPECT_FALSE(Exists(update_ + L"\\staged.json"));
}

// A swapped-in file that is gone when an interrupted swap's cleanup runs is not recorded as owned (a path with no sha256
// would make state.json unusable and have it rebuilt, losing the channel).
TEST_F(HostUpdateSwap, F7_ALeftoverDoneJournalOwnsOnlyTheSwappedFilesThatAreStillInTheRoot) {
  OldReleaseInTheRootAndANewOneStaged();
  ASSERT_EQ(Outcomes(RunStart().records), std::vector<std::string>{"swapped"});
  DeleteFileW((root_ + L"\\plugins\\example.dll").c_str());
  Put(L"update\\swap.json", Journal("done"));
  EXPECT_EQ(Outcomes(RunStart().records), std::vector<std::string>{"recovered"});
  const nlohmann::json state = ReadJson(update_ + L"\\state.json");
  EXPECT_EQ(state["owned"], nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(new_runtime_)}}}))
      << state.dump();
  EXPECT_EQ(state.value("channel", ""), "beta") << "the state was not rebuilt";
}

TEST_F(HostUpdateSwap, F7_ALeftoverDoneJournalKeepsThePendingRecordAndACrashMarkerMadeSince) {
  OldReleaseInTheRootAndANewOneStaged();
  ASSERT_EQ(Outcomes(RunStart().records), std::vector<std::string>{"swapped"});
  nlohmann::json pending = ReadJson(update_ + L"\\pending.json");
  ASSERT_EQ(pending["to"].value("version", ""), "1.2.3");
  pending["boot_attempts"] = 2;
  pending["failed_reason"] = "init_failed";
  Put(L"update\\pending.json", pending.dump());
  Put(L"update\\crashed", "");
  // The swapped runtime runs (it is mapped here), so the rollback these two records ask for waits (R2); what is under
  // test is that the cleanup itself resets neither.
  const HMODULE mapped = LoadLibraryExW(runtime_path_.c_str(), nullptr, 0);
  ASSERT_NE(mapped, nullptr) << GetLastError();
  for (int start = 1; start <= 2; ++start) {
    SCOPED_TRACE(start);
    Put(L"update\\swap.json", Journal("done"));
    EXPECT_EQ(Outcomes(RunStart().records), std::vector<std::string>{"recovered"});
    EXPECT_EQ(ReadJson(update_ + L"\\pending.json"), pending) << "not reset to boot_attempts 1";
    EXPECT_TRUE(Exists(update_ + L"\\crashed")) << "a crash of the swapped release is not forgotten";
    EXPECT_FALSE(Exists(update_ + L"\\swap.json"));
  }
  FreeLibrary(mapped);
}

// Review note 4: a staged.json the host cannot remove (another program holds it without delete sharing) is offered
// again at every start. Its release is quarantined once, and each record says the removal failed and why.
TEST_F(HostUpdateSwap, F6_AStagedJsonThatCannotBeRemovedIsQuarantinedOnceAndSaysWhy) {
  OldReleaseInTheRootAndANewOneStaged();
  WriteState({{"channel", "beta"}, {"owned", nlohmann::json::array()}, {"discards", {{"1.2.3", 2}}}});
  std::string changed = new_runtime_;
  changed[100] ^= 0x01;
  Put(L"update\\staging\\1.2.3\\nevr-runtime.dll", changed);
  Held held(update_ + L"\\staged.json", FILE_SHARE_READ);
  ASSERT_TRUE(held.ok());
  for (int start = 1; start <= 2; ++start) {
    SCOPED_TRACE(start);
    const auto records = SwapRecords(RunStart().records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "discarded");
    EXPECT_EQ(records[0].value("staged_removed", true), false);
    EXPECT_EQ(records[0].value("staged_win32_error", 0u), static_cast<unsigned>(ERROR_SHARING_VIOLATION));
  }
  const nlohmann::json state = ReadJson(update_ + L"\\state.json");
  ASSERT_TRUE(state["quarantine"].is_array()) << state.dump();
  EXPECT_EQ(state["quarantine"].size(), 1u) << state.dump();
  // The third discard quarantined the release; the offer that follows is dropped as a quarantined one, not counted again.
  EXPECT_EQ(state["discards"].value("1.2.3", 0), 3);
}

// The same for a bad staged.json the host cannot remove, and for a state.json it cannot write over (another program
// holds each without delete sharing): the record says so, with the error (logging.md Rule 10).
TEST_F(HostUpdateSwap, F22_ABadStagedJsonOrAStateJsonTheHostCannotReplaceIsReportedWithTheError) {
  Put(L"nevr-runtime.dll", old_runtime_);
  Put(L"update\\staged.json", "{\"version\": \"..\"}");
  {
    Held held(update_ + L"\\staged.json", FILE_SHARE_READ);
    ASSERT_TRUE(held.ok());
    const auto records = SwapRecords(RunStart().records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("reason", ""), "bad_staged");
    EXPECT_EQ(records[0].value("staged_removed", true), false);
    EXPECT_EQ(records[0].value("staged_win32_error", 0u), static_cast<unsigned>(ERROR_SHARING_VIOLATION));
  }
  RemoveTree(root_);
  ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
  OldReleaseInTheRootAndANewOneStaged();
  std::string changed = new_runtime_;
  changed[100] ^= 0x01;
  Put(L"update\\staging\\1.2.3\\nevr-runtime.dll", changed);
  Held held(update_ + L"\\state.json", FILE_SHARE_READ);
  ASSERT_TRUE(held.ok());
  const auto records = SwapRecords(RunStart().records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "discarded");
  EXPECT_EQ(records[0].value("state_written", true), false);
  EXPECT_NE(records[0].value("state_win32_error", 0u), 0u) << records[0].dump();
}

// Review note 7: a kill between a JSON file's temporary write and its rename leaves <file>.tmp-<pid>. Under the lock no
// writer is live, so a start removes the host's own leftovers, and only those.
TEST_F(HostUpdateSwap, AStartRemovesTheTemporaryFilesAKilledWriteLeftAndNothingElse) {
  Put(L"nevr-runtime.dll", old_runtime_);
  for (const wchar_t* ours : {L"state.json.tmp-1234", L"pending.json.tmp-7", L"swap.json.tmp-42"}) {
    Put(std::wstring(L"update\\") + ours, "{");
  }
  for (const wchar_t* theirs : {L"staged.json.tmp-5", L"state.json.tmp-12a", L"state.json.tmp-", L"notes.tmp-1"}) {
    Put(std::wstring(L"update\\") + theirs, "not the host's");
  }
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "recovered");
  EXPECT_EQ(records[0].value("reason", ""), "temporaries_removed");
  EXPECT_EQ(records[0].value("removed", 0), 3);
  for (const wchar_t* ours : {L"state.json.tmp-1234", L"pending.json.tmp-7", L"swap.json.tmp-42"}) {
    EXPECT_FALSE(Exists(update_ + L"\\" + ours)) << Narrow(ours);
  }
  for (const wchar_t* theirs : {L"staged.json.tmp-5", L"state.json.tmp-12a", L"state.json.tmp-", L"notes.tmp-1"}) {
    EXPECT_TRUE(Exists(update_ + L"\\" + theirs)) << Narrow(theirs);
  }
}

// Review note 8: a kill while a start recovers an earlier kill. Each recovery step is itself a step the seam can end;
// the start after it still ends with one whole release.
TEST_F(HostUpdateSwap, F7_AKillWhileRecoveringIsRecoveredInTurnByTheNextStart) {
  OldReleaseInTheRootAndANewOneStaged();
  RunStart();
  const int steps = g_last_step.load();
  int interrupted_recoveries = 0;
  for (int kill = 1; kill <= steps; ++kill) {
    for (int again = 1; again <= 6; ++again) {
      SCOPED_TRACE(std::to_string(kill) + " then " + std::to_string(again));
      if (KilledSwap(kill).empty()) break;  // nothing to recover after this kill
      g_last_step.store(0);
      g_kill_at.store(again);
      RunStart();
      g_kill_at.store(0);
      if (g_last_step.load() < again) break;  // this start has fewer steps
      ++interrupted_recoveries;
      const int attempts = AttemptsAfterTheNextStart();
      const Start last = RunStart();
      EXPECT_EQ(last.result.action, UpdateAction::kLoad);
      ExpectSwapped(attempts);
    }
  }
  // Every kill but the last (the journal's removal) leaves a journal, and each such recovery has at least one step.
  EXPECT_GE(interrupted_recoveries, steps - 1);
}

// ---- the pending release: rollback, the crash marker's reader, boot_attempts (#2105, ADR 0024 section 6) ---------------
//
// A swapped release is pending (update\pending.json) until the runtime confirms it started. These tests start from the
// usual swap done (SwappedIn) and change what happened to the release since: it crashed, failed, ran twice unconfirmed.

NevrHostContext ValidContext() {
  static unsigned char target[16] = {};
  NevrHostContext host{};
  host.header.struct_size = sizeof(NevrHostContext);
  host.header.abi_major = nevr_runtime_abi::kMajor;
  host.header.abi_minor = nevr_runtime_abi::kMinor;
  host.host_module = GetModuleHandleW(nullptr);
  host.game_module = GetModuleHandleW(nullptr);
  host.original_game_main_wrapper = +[](std::int64_t command_line) { return command_line; };
  host.wrapper_hook_target = target;
  host.wrapper_hook_bytes_valid = 0;
  host.capabilities = nevr_runtime_abi::kHostCapabilitySwap;
  return host;
}

struct LoadedStart {
  nevr_host::RuntimeLoadResult result;
  std::vector<nlohmann::json> records;
};

std::vector<std::string> Events(const std::vector<nlohmann::json>& records) {
  std::vector<std::string> out;
  for (const nlohmann::json& record : records) out.push_back(record.value("event", ""));
  return out;
}

std::vector<nlohmann::json> RollbackRecords(const std::vector<nlohmann::json>& records) {
  std::vector<nlohmann::json> out;
  for (const nlohmann::json& record : records) {
    if (record.value("event", "") == "update_rollback") out.push_back(record);
  }
  return out;
}

class HostUpdateRollback : public HostUpdateSwap {
 protected:
  // Rolling back restores the previous release, and the restored runtime passes the same verifier as any runtime (ADR 0024,
  // section 6): so here the release in the root before the swap is signed too. `old_runtime_` is a second signed runtime
  // stand-in (the rendezvous probe built another way, NEVR-BUILD 7.7.7, signed by the same throwaway PKI) that is never
  // loaded; `new_runtime_` is the one that loads; `bad_runtime_` is the one the verifier refuses.
  void SetUp() override {
    HostUpdateSwap::SetUp();
    if (HasFatalFailure()) return;
    ASSERT_TRUE(manifest_["files"].contains("nevr-runtime-b.signed")) << "the fixtures hold no second signed runtime";
    old_runtime_ = Fixture("nevr-runtime-b.signed");
    bad_runtime_ = Fixture("nevr-runtime.unsigned");
    ASSERT_FALSE(old_runtime_.empty());
    ASSERT_NE(old_runtime_, new_runtime_);
    ASSERT_NE(bad_runtime_, new_runtime_);
  }

  std::string bad_runtime_;

  // The usual swap, done: the new release is in the root, the old one in previous\7.7.7, pending.json says boot_attempts 1.
  void SwappedIn() {
    OldReleaseInTheRootAndANewOneStaged();
    const Start start = RunStart();
    ASSERT_EQ(Outcomes(start.records), std::vector<std::string>{"swapped"});
    ASSERT_TRUE(Exists(update_ + L"\\pending.json"));
    ASSERT_FALSE(Exists(update_ + L"\\crashed"));
  }

  nlohmann::json Pending() const { return ReadJson(update_ + L"\\pending.json"); }
  void SetPending(const nlohmann::json& pending) { Put(L"update\\pending.json", pending.dump()); }
  void MarkCrashed() { Put(L"update\\crashed", ""); }

  // What the root holds after the swapped release was put back: the old release in place, the new one kept under
  // failed\1.2.3, the pending record and the marker gone, state.json quarantining the failed release.
  void ExpectRolledBack() {
    EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_) << "the previous runtime is back";
    EXPECT_EQ(Get(L"plugins\\example.dll"), old_plugin_) << "and its plugin";
    EXPECT_EQ(Get(L"update\\failed\\1.2.3\\nevr-runtime.dll"), new_runtime_) << "the failed runtime is kept for the report";
    EXPECT_EQ(Get(L"update\\failed\\1.2.3\\plugins\\example.dll"), new_plugin_);
    EXPECT_FALSE(Exists(update_ + L"\\previous\\7.7.7")) << "the previous generation moved back, nothing is left";
    EXPECT_FALSE(Exists(update_ + L"\\pending.json"));
    EXPECT_FALSE(Exists(update_ + L"\\crashed"));
    EXPECT_FALSE(Exists(update_ + L"\\swap.json"));
    const nlohmann::json state = ReadJson(update_ + L"\\state.json");
    EXPECT_EQ(state.value("channel", ""), "beta") << state.dump();
    ASSERT_TRUE(state["quarantine"].is_array()) << state.dump();
    ASSERT_EQ(state["quarantine"].size(), 1u) << state.dump();
    EXPECT_EQ(state["quarantine"][0].value("version", ""), "1.2.3");
    EXPECT_EQ(state["quarantine"][0].value("runtime_sha256", ""), Sha256Hex(new_runtime_));
    EXPECT_FALSE(state["quarantine"][0].value("reason", "").empty());
    EXPECT_FALSE(state["quarantine"][0].value("time", "").empty());
    EXPECT_EQ(state["current"], nlohmann::json({{"version", "7.7.7"}, {"runtime_sha256", Sha256Hex(old_runtime_)}}))
        << state.dump();
    EXPECT_EQ(state["owned"], nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(old_runtime_)}},
                                                     {{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(old_plugin_)}}}))
        << state.dump();
  }

  LoadedStart LoadAtStart(const NevrHostContext& context) {
    {
      std::lock_guard<std::mutex> lock(g_records_mutex);
      g_records.clear();
    }
    nevr_host::SetRuntimeDirForTest(root_.c_str());
    nevr_host::SetVerifyRuntimeForTest(true);
    nevr_host::SetDevBuildForTest(0);
    LoadedStart run;
    {
      BootstrapDiagnostics diagnostics;
      const std::wstring host_dir = scratch_ + L"\\";
      diagnostics.PrepareForTest(local_.c_str(), host_dir.c_str());
      nevr_host::RuntimeLoader loader;
      loader.Prepare();
      run.result = loader.LoadAtStart(context, diagnostics);
    }
    nevr_host::SetRuntimeDirForTest(nullptr);
    nevr_host::SetVerifyRuntimeForTest(false);
    nevr_host::SetDevBuildForTest(-1);
    std::lock_guard<std::mutex> lock(g_records_mutex);
    run.records = g_records;
    return run;
  }

  // A pending release whose runtime the verifier refuses (the unsigned stand-in), with the signed one that loads kept as
  // the previous release: what the root holds after a swap whose new runtime turned out to be unusable.
  void PendingReleaseThatTheVerifierRefuses() {
    Put(L"nevr-runtime.dll", bad_runtime_);  // unsigned: refused
    Put(L"plugins\\example.dll", old_plugin_);
    Put(L"update\\previous\\7.7.7\\nevr-runtime.dll", new_runtime_);  // signed, and it loads
    Put(L"update\\previous\\7.7.7\\plugins\\example.dll", new_plugin_);
    WriteState({{"channel", "beta"},
                {"owned", {{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(bad_runtime_)}},
                           {{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(old_plugin_)}}}}});
    SetPending({{"to", {{"version", "1.2.3"}, {"sha256", Sha256Hex(bad_runtime_)}}},
                {"from", {{"version", "7.7.7"}, {"sha256", Sha256Hex(new_runtime_)}}},
                {"boot_attempts", 1},
                {"failed_reason", nullptr}});
  }
};

TEST_F(HostUpdateRollback, ASwapLeavesTheReleasePendingAndTellsTheLoaderSo) {
  OldReleaseInTheRootAndANewOneStaged();
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.result.pending) << "a load of the swapped-in release that fails is the host's to handle";
  const Start quiet = [&] {
    RemoveTree(root_);
    CreateDirectoryW(root_.c_str(), nullptr);
    Put(L"nevr-runtime.dll", old_runtime_);
    MakeDirs(update_);
    return RunStart();
  }();
  EXPECT_FALSE(quiet.result.pending) << "an update folder with nothing pending is not a trial";
}

// F12: a release gets two starts to reach the mark; the third rolls back.
TEST_F(HostUpdateRollback, F12_AReleaseThatNeverReachesTheMarkInTwoStartsIsRolledBackAtTheThird) {
  SwappedIn();
  EXPECT_EQ(Pending().value("boot_attempts", 0), 1);

  const Start second = RunStart();
  EXPECT_EQ(second.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(second.result.pending);
  EXPECT_EQ(Pending().value("boot_attempts", 0), 2) << "the second start is the release's second try";
  EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_) << "and it loads the pending release";
  EXPECT_TRUE(RollbackRecords(second.records).empty());

  const Start third = RunStart();
  EXPECT_EQ(third.result.action, UpdateAction::kLoad);
  EXPECT_FALSE(third.result.pending) << "the previous release is in the root; it is not on trial";
  EXPECT_TRUE(third.lock_held);
  ExpectRolledBack();
  const auto records = RollbackRecords(third.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("mode", ""), "next_start");
  EXPECT_EQ(records[0].value("reason", ""), "unconfirmed");
  EXPECT_EQ(records[0].value("boot_attempts", 0), 2);
  EXPECT_EQ(records[0].value("outcome", ""), "rolled_back");
  EXPECT_EQ(records[0].value("from", ""), "1.2.3");
  EXPECT_EQ(records[0].value("to", ""), "7.7.7");
  EXPECT_TRUE(records[0].contains("elapsed_ms"));
}

// F11: the crash capture left update\crashed; the next start rolls back at once.
TEST_F(HostUpdateRollback, F11_ACrashMarkerRollsTheReleaseBackAtTheNextStart) {
  SwappedIn();
  MarkCrashed();
  const Start next = RunStart();
  EXPECT_EQ(next.result.action, UpdateAction::kLoad);
  ExpectRolledBack();
  const auto records = RollbackRecords(next.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("reason", ""), "crashed");
  EXPECT_EQ(records[0].value("outcome", ""), "rolled_back");
}

// F10: a runtime that was mapped and failed has its reason in pending.json; the next start rolls back with it.
TEST_F(HostUpdateRollback, F10_AFailedReasonInPendingJsonRollsBackWithThatReasonAndBeatsTheOtherTwo) {
  SwappedIn();
  nlohmann::json pending = Pending();
  pending["failed_reason"] = "abi_rejected";
  pending["boot_attempts"] = 2;
  SetPending(pending);
  MarkCrashed();
  const Start next = RunStart();
  ExpectRolledBack();
  const auto records = RollbackRecords(next.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("reason", ""), "abi_rejected") << "the release's own failure is named before a crash or a count";
  EXPECT_EQ(ReadJson(update_ + L"\\state.json")["quarantine"][0].value("reason", ""), "abi_rejected");
}

// A second start inside the first one's trial must not condemn the release (ADR 0024, section 6, R2).
TEST_F(HostUpdateRollback, ASecondStartInsideTheFirstOnesTrialDoesNotRollBackAndDoesNotCountAsATry) {
  SwappedIn();
  const HMODULE mapped = LoadLibraryExW(runtime_path_.c_str(), nullptr, 0);  // the first start's runtime, in its ten seconds
  ASSERT_NE(mapped, nullptr) << GetLastError();
  // Two reasons to roll back are present; the runtime is in use, so neither acts.
  MarkCrashed();
  nlohmann::json pending = Pending();
  pending["boot_attempts"] = 2;
  SetPending(pending);
  const auto before = Tree(root_);
  const Start inside = RunStart();
  EXPECT_EQ(inside.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(inside.result.pending) << "it loads the pending release and decides at a later start";
  EXPECT_EQ(Tree(root_), before) << "nothing moved, nothing was counted";
  const auto records = RollbackRecords(inside.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "deferred");
  EXPECT_EQ(records[0].value("reason", ""), "runtime_in_use");

  // One try so far and a second process arrives while the first still runs: not a try either.
  pending["boot_attempts"] = 1;
  SetPending(pending);
  DeleteFileW((update_ + L"\\crashed").c_str());
  const Start overlapping = RunStart();
  EXPECT_TRUE(overlapping.result.pending);
  EXPECT_EQ(Pending().value("boot_attempts", 0), 1) << "a start beside a running one is not a second try";

  FreeLibrary(mapped);
  pending["boot_attempts"] = 2;
  SetPending(pending);
  RunStart();
  ExpectRolledBack();
}

// F13: nothing to restore, or the restored file does not verify: nothing that cannot be verified is loaded.
TEST_F(HostUpdateRollback, F13_ARollbackWithNothingToRestoreStopsAndLeavesTheRootAsItWas) {
  for (const char* why : {"no_runtime", "no_pending_from", "unsigned"}) {
    SCOPED_TRACE(why);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    SwappedIn();
    MarkCrashed();
    if (std::string(why) == "no_runtime") DeleteFileW((update_ + L"\\previous\\7.7.7\\nevr-runtime.dll").c_str());
    if (std::string(why) == "no_pending_from") {
      nlohmann::json pending = Pending();
      pending["from"] = nullptr;
      SetPending(pending);
    }
    if (std::string(why) == "unsigned") Put(L"update\\previous\\7.7.7\\nevr-runtime.dll", bad_runtime_);
    const auto before = Tree(root_);
    const Start next = RunStart();
    EXPECT_EQ(next.result.action, UpdateAction::kStop);
    ASSERT_NE(next.result.reason, nullptr);
    EXPECT_STREQ(next.result.reason, std::string(why) == "unsigned" ? "previous_unverified" : "no_previous");
    EXPECT_EQ(Tree(root_), before) << "nothing moved";
    const auto records = RollbackRecords(next.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "failed");
    EXPECT_EQ(records[0].value("reason", ""), next.result.reason);
  }
}

// F20: a rollback is never undone; whichever step ends it, the next start completes it and the failed release stays out.
TEST_F(HostUpdateRollback, F20_AnInterruptedRollbackIsCompletedByTheNextStartAndTheFailedReleaseDoesNotReturn) {
  SwappedIn();
  MarkCrashed();
  g_last_step.store(0);
  RunStart();
  const int steps = g_last_step.load();
  ASSERT_GE(steps, 10) << "the journal, four moves, the state, two removals and the cleanup";
  ExpectRolledBack();
  bool saw_a_journal = false;
  for (int kill = 1; kill <= steps; ++kill) {
    SCOPED_TRACE(kill);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    SwappedIn();
    MarkCrashed();
    g_kill_at.store(kill);
    RunStart();
    g_kill_at.store(0);
    const bool journal = Exists(update_ + L"\\swap.json");
    const nlohmann::json written = ReadJson(update_ + L"\\swap.json");
    if (journal) {
      saw_a_journal = true;
      EXPECT_EQ(written.value("kind", ""), "rollback");
    }
    const Start next = RunStart();
    EXPECT_EQ(next.result.action, UpdateAction::kLoad);
    EXPECT_FALSE(next.result.pending) << "the failed release does not come back as the pending one";
    ExpectRolledBack();
    if (journal) {
      const auto outcomes = Outcomes(next.records);
      ASSERT_FALSE(outcomes.empty());
      EXPECT_EQ(outcomes.front(), "recovered");
      EXPECT_EQ(SwapRecords(next.records).front().value("kind", ""), "rollback");
    }
    const Start after = RunStart();
    EXPECT_TRUE(SwapRecords(after.records).empty() && RollbackRecords(after.records).empty())
        << "the start after that finds nothing to do";
  }
  EXPECT_TRUE(saw_a_journal);
}

// A plugin set is rolled back with the runtime, and only the files the release owned.
TEST_F(HostUpdateRollback, APluginSetIsRolledBackWithTheRuntimeAndAStrangersPluginStays) {
  Put(L"nevr-runtime.dll", old_runtime_);
  Put(L"plugins\\example.dll", old_plugin_);
  Put(L"plugins\\mine.dll", "a player's own plugin");
  WriteState({{"channel", "beta"}, {"owned", {{{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(old_plugin_)}}}}});
  Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_},
                  {"plugins/example.dll", new_plugin_},
                  {"plugins/extra.dll", Fixture("good")}});
  ASSERT_EQ(Outcomes(RunStart().records), std::vector<std::string>{"swapped"});
  ASSERT_EQ(Get(L"plugins\\extra.dll"), Fixture("good"));
  MarkCrashed();
  const Start next = RunStart();
  EXPECT_EQ(next.result.action, UpdateAction::kLoad);
  EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_);
  EXPECT_EQ(Get(L"plugins\\example.dll"), old_plugin_);
  EXPECT_FALSE(Exists(root_ + L"\\plugins\\extra.dll")) << "a plugin only the failed release had goes with it";
  EXPECT_EQ(Get(L"update\\failed\\1.2.3\\plugins\\extra.dll"), Fixture("good"));
  EXPECT_EQ(Get(L"update\\failed\\1.2.3\\plugins\\example.dll"), new_plugin_);
  EXPECT_EQ(Get(L"plugins\\mine.dll"), "a player's own plugin") << "no stranger's file is moved";
  EXPECT_EQ(ReadJson(update_ + L"\\state.json")["owned"],
            nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(old_runtime_)}},
                                   {{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(old_plugin_)}}}));
}

TEST_F(HostUpdateRollback, AnOlderFailedGenerationIsReplacedAndAFileOfOursIsNotOverwritten) {
  SwappedIn();
  MarkCrashed();
  Put(L"update\\failed\\0.0.9\\nevr-runtime.dll", "an older failed release");
  RunStart();
  ExpectRolledBack();
  EXPECT_FALSE(Exists(update_ + L"\\failed\\0.0.9")) << "one failed generation is kept";
}

// Q1: a staged release waits while one is pending; it would move an unconfirmed release over the last confirmed one.
TEST_F(HostUpdateRollback, Q1_AStagedReleaseWaitsWhileAReleaseIsPending) {
  SwappedIn();
  Stage("1.3.0", {{"nevr-runtime.dll", new_runtime_}});
  const Start next = RunStart();
  EXPECT_EQ(next.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(next.result.pending);
  EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_) << "the pending release stays";
  EXPECT_EQ(Get(L"update\\previous\\7.7.7\\nevr-runtime.dll"), old_runtime_) << "the last confirmed release is not deleted";
  EXPECT_TRUE(Exists(update_ + L"\\staged.json")) << "and the staged one stays for a later start";
  bool deferred = false;
  for (const nlohmann::json& record : SwapRecords(next.records)) {
    if (record.value("outcome", "") == "deferred" && record.value("reason", "") == "release_pending") deferred = true;
  }
  EXPECT_TRUE(deferred);
}

// A release the host rolled back is not offered again: the same version and runtime staged anyway is dropped.
TEST_F(HostUpdateRollback, AStagedReleaseThatWasQuarantinedIsDroppedAndNeverSwappedIn) {
  OldReleaseInTheRootAndANewOneStaged();
  nlohmann::json state = ReadJson(update_ + L"\\state.json");
  state["quarantine"] = nlohmann::json::array(
      {{{"version", "1.2.3"}, {"runtime_sha256", Sha256Hex(new_runtime_)}, {"reason", "crashed"}, {"time", "2026-11-05T00:00:00Z"}}});
  WriteState(state);
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_FALSE(start.result.pending);
  EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_);
  EXPECT_FALSE(Exists(update_ + L"\\staged.json"));
  EXPECT_FALSE(Exists(update_ + L"\\pending.json"));
  const auto records = SwapRecords(start.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "discarded");
  EXPECT_EQ(records[0].value("reason", ""), "quarantined");
  EXPECT_FALSE(ReadJson(update_ + L"\\state.json").contains("discards")) << "a dropped release is not a discard";
}

// A pending.json that names a runtime other than the one in the root is stale: the host moves no file it did not place.
TEST_F(HostUpdateRollback, APendingRecordForARuntimeThatIsNotInTheRootIsDroppedAndNothingMoves) {
  SwappedIn();
  MarkCrashed();
  Put(L"nevr-runtime.dll", RuntimeNamed("9.9.9"));  // someone put another runtime there
  const std::string other = Get(L"nevr-runtime.dll");
  const Start next = RunStart();
  EXPECT_EQ(next.result.action, UpdateAction::kLoad);
  EXPECT_FALSE(next.result.pending);
  EXPECT_EQ(Get(L"nevr-runtime.dll"), other);
  EXPECT_FALSE(Exists(update_ + L"\\pending.json"));
  EXPECT_FALSE(Exists(update_ + L"\\crashed"));
  EXPECT_EQ(Get(L"update\\previous\\7.7.7\\nevr-runtime.dll"), old_runtime_) << "the previous generation is left alone";
  const auto records = RollbackRecords(next.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("outcome", ""), "discarded");
  EXPECT_EQ(records[0].value("reason", ""), "pending_stale");
}

TEST_F(HostUpdateRollback, APendingRecordTheHostCannotParseIsDroppedAndOneItCannotReadIsLeftAlone) {
  SwappedIn();
  Put(L"update\\pending.json", "{\"to\": {\"version\": \"..\\\\..\\\\x\", \"sha256\": \"zz\"}}");
  Start next = RunStart();
  EXPECT_EQ(next.result.action, UpdateAction::kLoad);
  EXPECT_FALSE(Exists(update_ + L"\\pending.json"));
  auto records = RollbackRecords(next.records);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].value("reason", ""), "bad_pending");
  EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_) << "nothing moved";

  RemoveTree(root_);
  ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
  SwappedIn();
  MarkCrashed();
  auto before = Tree(root_);
  const nlohmann::json pending_before = Pending();
  before.erase("update\\pending.json");  // the held file cannot be read by the comparison either; it is compared below
  {
    Held held(update_ + L"\\pending.json", 0);
    ASSERT_TRUE(held.ok());
    next = RunStart();
    EXPECT_EQ(next.result.action, UpdateAction::kLoad);
    auto after = Tree(root_);
    after.erase("update\\pending.json");
    EXPECT_EQ(after, before) << "a record another program holds is neither judged nor changed";
    records = RollbackRecords(next.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "deferred");
    EXPECT_EQ(records[0].value("reason", ""), "pending_unreadable");
    EXPECT_GT(records[0].value("win32_error", 0u), 0u);
  }
  EXPECT_EQ(Pending(), pending_before) << "and the record itself is as it was";
}

// A restored file that is gone again when the rollback is completed (a kill, then a player removing it) is skipped, not
// a failed move, and is not recorded as owned (a path with no sha256 would make state.json unusable).
TEST_F(HostUpdateRollback, ACompletedRollbackSkipsARestoredFileThatIsGoneAndOwnsOnlyWhatIsThere) {
  SwappedIn();
  nlohmann::json journal = {{"kind", "rollback"}, {"from", "1.2.3"}, {"to", "7.7.7"}, {"phase", "in"},
                            {"out", {"nevr-runtime.dll", "plugins/example.dll"}},
                            {"in", {"nevr-runtime.dll", "plugins/example.dll"}},
                            {"failed_sha256", Sha256Hex(new_runtime_)}, {"reason", "crashed"}};
  // The state a kill left after the failed files moved out and the runtime came back; the plugin is gone from both places.
  Put(L"update\\failed\\1.2.3\\nevr-runtime.dll", new_runtime_);
  Put(L"nevr-runtime.dll", old_runtime_);
  DeleteFileW((root_ + L"\\plugins\\example.dll").c_str());
  DeleteFileW((update_ + L"\\previous\\7.7.7\\plugins\\example.dll").c_str());
  Put(L"update\\swap.json", journal.dump());
  const Start next = RunStart();
  EXPECT_EQ(next.result.action, UpdateAction::kLoad);
  EXPECT_FALSE(Exists(update_ + L"\\swap.json"));
  EXPECT_FALSE(Exists(update_ + L"\\pending.json"));
  EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_);
  const nlohmann::json state = ReadJson(update_ + L"\\state.json");
  EXPECT_EQ(state["owned"], nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(old_runtime_)}}}))
      << state.dump();
  ASSERT_EQ(state["quarantine"].size(), 1u) << state.dump();
  EXPECT_EQ(Outcomes(next.records), std::vector<std::string>{"recovered"});
}

TEST_F(HostUpdateRollback, F22_ARollbackJournalWithoutItsFailedReleaseOrAReasonIsNotExecuted) {
  SwappedIn();
  const nlohmann::json whole = {{"kind", "rollback"}, {"from", "1.2.3"}, {"to", "7.7.7"}, {"phase", "out"},
                                {"out", {"nevr-runtime.dll"}}, {"in", {"nevr-runtime.dll"}},
                                {"failed_sha256", Sha256Hex(new_runtime_)}, {"reason", "crashed"}};
  for (const char* broken : {"failed_sha256", "reason"}) {
    SCOPED_TRACE(broken);
    nlohmann::json journal = whole;
    journal.erase(broken);
    Put(L"update\\swap.json", journal.dump());
    const auto before = Tree(root_);
    const Start next = RunStart();
    EXPECT_EQ(next.result.action, UpdateAction::kStop);
    ASSERT_NE(next.result.reason, nullptr);
    EXPECT_STREQ(next.result.reason, "bad_journal");
    EXPECT_EQ(Tree(root_), before);
  }
  for (const nlohmann::json& bad : {nlohmann::json{{"reason", "Crashed Hard"}}, nlohmann::json{{"failed_sha256", "xyz"}}}) {
    nlohmann::json journal = whole;
    journal.update(bad);
    Put(L"update\\swap.json", journal.dump());
    EXPECT_EQ(RunStart().result.action, UpdateAction::kStop) << bad.dump();
  }
}

// The first failure is the cause that stays: a second mark does not replace it.
TEST_F(HostUpdateRollback, TheFirstFailureMarkedOnAPendingReleaseIsTheOneThatStays) {
  SwappedIn();
  BootstrapDiagnostics diagnostics;
  const std::wstring host_dir = scratch_ + L"\\";
  diagnostics.PrepareForTest(local_.c_str(), host_dir.c_str());
  UpdateLock lock;
  EXPECT_TRUE(nevr_host::MarkPendingFailed(runtime_path_.c_str(), diagnostics, lock, "abi_rejected"));
  EXPECT_EQ(Pending().value("failed_reason", ""), "abi_rejected");
  EXPECT_TRUE(nevr_host::MarkPendingFailed(runtime_path_.c_str(), diagnostics, lock, "init_failed"));
  EXPECT_EQ(Pending().value("failed_reason", ""), "abi_rejected") << "the first cause stays";
  EXPECT_FALSE(lock.Held()) << "the lock is not kept";
}

// ---- the loader: R1 in the same start, and the mark for the next ------------------------------------------------

// F9: the new runtime never ran (the verifier refuses it): the previous one is put back and the game starts on it.
TEST_F(HostUpdateRollback, F9_ARefusedPendingRuntimeIsReplacedByThePreviousOneInTheSameStart) {
  PendingReleaseThatTheVerifierRefuses();
  const LoadedStart run = LoadAtStart(ValidContext());
  EXPECT_EQ(run.result.outcome, nevr_host::RuntimeLoadOutcome::kLoaded) << run.result.OutcomeName();
  if (run.result.module != nullptr) FreeLibrary(run.result.module);
  EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_) << "the previous release is in the root and was loaded";
  EXPECT_EQ(Get(L"plugins\\example.dll"), new_plugin_);
  EXPECT_EQ(Get(L"update\\failed\\1.2.3\\nevr-runtime.dll"), bad_runtime_);
  EXPECT_FALSE(Exists(update_ + L"\\pending.json"));
  const nlohmann::json state = ReadJson(update_ + L"\\state.json");
  ASSERT_EQ(state["quarantine"].size(), 1u) << state.dump();
  EXPECT_EQ(state["quarantine"][0].value("runtime_sha256", ""), Sha256Hex(bad_runtime_));
  EXPECT_EQ(state["quarantine"][0].value("reason", ""), "runtime_refused");
  // The second verdict is its own versions record, with update_rollback between the two (ADR 0024, section 8).
  EXPECT_EQ(Events(run.records), (std::vector<std::string>{"update_trial", "versions", "runtime_load", "update_rollback",
                                                          "versions", "runtime_load"}));
  const auto rollbacks = RollbackRecords(run.records);
  ASSERT_EQ(rollbacks.size(), 1u);
  EXPECT_EQ(rollbacks[0].value("mode", ""), "in_process");
  EXPECT_EQ(rollbacks[0].value("reason", ""), "runtime_refused");
  EXPECT_EQ(rollbacks[0].value("outcome", ""), "rolled_back");
  EXPECT_EQ(run.records[0].value("outcome", ""), "counted") << "the start that loaded the pending release was its second try";
  EXPECT_EQ(run.records[2].value("outcome", ""), "runtime_refused");
  EXPECT_EQ(run.records[5].value("outcome", ""), "loaded");
}

TEST_F(HostUpdateRollback, F9_ARuntimeThatIsMissingFromThePendingReleaseIsReplacedToo) {
  PendingReleaseThatTheVerifierRefuses();
  DeleteFileW(runtime_path_.c_str());
  const LoadedStart run = LoadAtStart(ValidContext());
  if (run.result.module != nullptr) FreeLibrary(run.result.module);
  EXPECT_EQ(run.result.outcome, nevr_host::RuntimeLoadOutcome::kLoaded) << run.result.OutcomeName();
  EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_);
  const auto rollbacks = RollbackRecords(run.records);
  ASSERT_EQ(rollbacks.size(), 1u);
  EXPECT_EQ(rollbacks[0].value("reason", ""), "runtime_absent");
}

TEST_F(HostUpdateRollback, F13_AnInProcessRollbackWithNothingToRestoreStopsTheStartWithTheCause) {
  PendingReleaseThatTheVerifierRefuses();
  DeleteFileW((update_ + L"\\previous\\7.7.7\\nevr-runtime.dll").c_str());
  const LoadedStart run = LoadAtStart(ValidContext());
  EXPECT_EQ(run.result.outcome, nevr_host::RuntimeLoadOutcome::kUpdateBlocked);
  ASSERT_NE(run.result.update_reason, nullptr);
  EXPECT_STREQ(run.result.update_reason, "no_previous");
  EXPECT_EQ(run.result.module, nullptr) << "nothing it cannot verify is loaded";
  EXPECT_EQ(Get(L"nevr-runtime.dll"), bad_runtime_) << "the root is as it was";
  EXPECT_EQ(Outcomes(run.records), std::vector<std::string>{}) << "update_swap records only; the stop is the rollback's";
  const auto rollbacks = RollbackRecords(run.records);
  ASSERT_EQ(rollbacks.size(), 1u);
  EXPECT_EQ(rollbacks[0].value("outcome", ""), "failed");
  EXPECT_EQ(rollbacks[0].value("reason", ""), "no_previous");
}

// F10 end to end: the swapped runtime is mapped and its initialize refuses the host (a zeroed context stands in for an
// ABI the runtime cannot take): the start fails as it does today, the release is marked, the next start puts it back.
TEST_F(HostUpdateRollback, F10_AMappedRuntimeThatFailsIsMarkedAndTheNextStartPutsThePreviousOneBack) {
  OldReleaseInTheRootAndANewOneStaged();
  const NevrHostContext refused{};
  const LoadedStart run = LoadAtStart(refused);
  EXPECT_EQ(run.result.outcome, nevr_host::RuntimeLoadOutcome::kInitFailed);
  EXPECT_TRUE(run.result.update_failed) << "the stop notice says the release was updated, failed, and comes back";
  ASSERT_NE(run.result.module, nullptr);
  EXPECT_EQ(Pending().value("failed_reason", ""), "init_failed");
  EXPECT_EQ(Pending().value("boot_attempts", 0), 1);
  EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_) << "this start does not touch the mapped file";
  const auto scheduled = RollbackRecords(run.records);
  ASSERT_EQ(scheduled.size(), 1u);
  EXPECT_EQ(scheduled[0].value("mode", ""), "next_start");
  EXPECT_EQ(scheduled[0].value("outcome", ""), "scheduled");
  EXPECT_EQ(scheduled[0].value("reason", ""), "init_failed");
  EXPECT_FALSE(Exists(update_ + L"\\swap.json"));

  FreeLibrary(run.result.module);  // the process that ends takes the mapping with it
  const Start next = RunStart();
  EXPECT_EQ(next.result.action, UpdateAction::kLoad);
  ExpectRolledBack();
  const auto done = RollbackRecords(next.records);
  ASSERT_EQ(done.size(), 1u);
  EXPECT_EQ(done[0].value("reason", ""), "init_failed");
  EXPECT_EQ(done[0].value("outcome", ""), "rolled_back");
}

TEST_F(HostUpdateRollback, AReleaseThatStartsIsLeftPendingForTheRuntimeToConfirm) {
  OldReleaseInTheRootAndANewOneStaged();
  const LoadedStart run = LoadAtStart(ValidContext());
  EXPECT_EQ(run.result.outcome, nevr_host::RuntimeLoadOutcome::kLoaded) << run.result.OutcomeName();
  EXPECT_FALSE(run.result.update_failed);
  if (run.result.module != nullptr) FreeLibrary(run.result.module);
  EXPECT_TRUE(Exists(update_ + L"\\pending.json")) << "only the runtime's UpdateConfirm removes it";
  EXPECT_TRUE(Pending()["failed_reason"].is_null());
  EXPECT_TRUE(RollbackRecords(run.records).empty());
  EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_);
}

// ---- review of #2204: records that must survive a held file, refusals that must fire, records for every start ----------

// A file another program holds open only while the swap finishes (a virus scanner, a backup tool): `HoldWhile` opens
// `hold` with share mode 0 the first time the step seam is called with `when` there, and keeps it until Release.
std::wstring g_hold_path;
std::wstring g_hold_when;
HANDLE g_hold_handle = INVALID_HANDLE_VALUE;

bool HoldAt(int step) {
  if (g_hold_handle == INVALID_HANDLE_VALUE && Exists(g_hold_when) && Exists(g_hold_path)) {
    g_hold_handle = CreateFileW(g_hold_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  return KillAt(step);
}

// The same, once the journal at `g_hold_when` says the swap reached its cleanup.
bool HoldAtDone(int step) {
  if (g_hold_handle == INVALID_HANDLE_VALUE && ReadAll(g_hold_when).find("\"phase\":\"done\"") != std::string::npos &&
      Exists(g_hold_path)) {
    g_hold_handle = CreateFileW(g_hold_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  return KillAt(step);
}

bool ReleaseHold() {
  const bool held = g_hold_handle != INVALID_HANDLE_VALUE;
  if (held) CloseHandle(g_hold_handle);
  g_hold_handle = INVALID_HANDLE_VALUE;
  nevr_host::SetUpdateStepHookForTest(&KillAt);
  return held;
}

// The pending record is built from facts the swap verified, not from a re-read that can fail: a previous runtime held
// open while the swap finishes is still recorded, so the rollback finds it (review finding 1).
TEST_F(HostUpdateRollback, APreviousRuntimeHeldWhileTheSwapFinishesIsStillPutBack) {
  OldReleaseInTheRootAndANewOneStaged();
  g_hold_path = update_ + L"\\previous\\7.7.7\\nevr-runtime.dll";
  g_hold_when = runtime_path_;
  nevr_host::SetUpdateStepHookForTest(&HoldAt);
  const Start swap = RunStart();
  ASSERT_TRUE(ReleaseHold()) << "the hold never took";
  ASSERT_EQ(Outcomes(swap.records), std::vector<std::string>{"swapped"});
  EXPECT_EQ(Get(L"update\\previous\\7.7.7\\nevr-runtime.dll"), old_runtime_);
  EXPECT_TRUE(Pending()["from"].is_object()) << "pending.json: " << Pending().dump();
  MarkCrashed();
  const Start next = RunStart();
  EXPECT_EQ(next.result.action, UpdateAction::kLoad) << (next.result.reason != nullptr ? next.result.reason : "");
  ExpectRolledBack();
}

TEST_F(HostUpdateRollback, ARuntimeHeldWhileTheSwapFinishesIsStillOnTrialWithItsSha256) {
  OldReleaseInTheRootAndANewOneStaged();
  g_hold_path = runtime_path_;
  g_hold_when = update_ + L"\\previous\\7.7.7\\nevr-runtime.dll";
  nevr_host::SetUpdateStepHookForTest(&HoldAt);
  const Start swap = RunStart();
  ASSERT_TRUE(ReleaseHold()) << "the hold never took";
  ASSERT_EQ(Outcomes(swap.records), std::vector<std::string>{"swapped"});
  EXPECT_EQ(Pending()["to"].value("sha256", ""), Sha256Hex(new_runtime_)) << "pending.json: " << Pending().dump();
  const Start second = RunStart();
  EXPECT_EQ(second.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(Exists(update_ + L"\\pending.json"));
  for (const nlohmann::json& record : RollbackRecords(second.records)) {
    EXPECT_NE(record.value("reason", ""), "bad_pending") << record.dump();
  }
  MarkCrashed();
  RunStart();
  EXPECT_EQ(Get(L"nevr-runtime.dll"), old_runtime_) << "a crash of the pending release is rolled back";
}

// A swapped-in file held open while the cleanup runs is not dropped from `owned` as if it were gone.
TEST_F(HostUpdateRollback, APluginHeldWhileTheSwapFinishesIsStillOwned) {
  OldReleaseInTheRootAndANewOneStaged();
  g_hold_path = root_ + L"\\plugins\\example.dll";
  g_hold_when = update_ + L"\\swap.json";
  nevr_host::SetUpdateStepHookForTest(&HoldAtDone);
  const Start swap = RunStart();
  ASSERT_TRUE(ReleaseHold()) << "the hold never took";
  ASSERT_EQ(Outcomes(swap.records), std::vector<std::string>{"swapped"});
  const nlohmann::json state = ReadJson(update_ + L"\\state.json");
  bool plugin_owned = false;
  for (const nlohmann::json& entry : state["owned"]) {
    if (entry.value("path", "") == "plugins/example.dll") plugin_owned = entry.value("sha256", "") == Sha256Hex(new_plugin_);
  }
  EXPECT_TRUE(plugin_owned) << state.dump();
}

// RollBackInProcess's doc says a stop that leaves the failed release in the root marks it, so the next start tries
// again: every stop does (review finding 5).
TEST_F(HostUpdateRollback, AnInProcessRollbackThatStopsMarksTheFailedRelease) {
  for (const char* why : {"no_previous", "previous_unverified", "rollback_blocked"}) {
    SCOPED_TRACE(why);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    PendingReleaseThatTheVerifierRefuses();
    if (std::string(why) == "no_previous") DeleteFileW((update_ + L"\\previous\\7.7.7\\nevr-runtime.dll").c_str());
    if (std::string(why) == "previous_unverified") Put(L"update\\previous\\7.7.7\\nevr-runtime.dll", bad_runtime_);
    if (std::string(why) == "rollback_blocked") {
      ASSERT_FALSE(LinkAway(L"update\\previous\\7.7.7\\plugins").empty());
    }
    const LoadedStart run = LoadAtStart(ValidContext());
    ASSERT_EQ(run.result.outcome, nevr_host::RuntimeLoadOutcome::kUpdateBlocked);
    ASSERT_NE(run.result.update_reason, nullptr);
    EXPECT_STREQ(run.result.update_reason, why);
    EXPECT_EQ(Pending().value("failed_reason", nlohmann::json()), nlohmann::json("runtime_refused")) << Pending().dump();
  }
}

// A start on a pending release says which try it is, or why it is not one (ADR 0024, requirement 4).
TEST_F(HostUpdateRollback, ATrialStartSaysWhichTryItIsAndWhyAnotherIsNotOne) {
  SwappedIn();
  const Start second = RunStart();
  ASSERT_EQ(Pending().value("boot_attempts", 0), 2);
  std::vector<nlohmann::json> trials;
  for (const nlohmann::json& record : second.records) {
    if (record.value("event", "") == "update_trial") trials.push_back(record);
  }
  ASSERT_EQ(trials.size(), 1u) << "boot_attempts went 1 -> 2 on disk with no record";
  EXPECT_EQ(trials[0].value("outcome", ""), "counted");
  EXPECT_EQ(trials[0].value("boot_attempts", 0), 2);
  EXPECT_EQ(trials[0].value("release", ""), "1.2.3");

  const HMODULE mapped = LoadLibraryExW(runtime_path_.c_str(), nullptr, 0);
  ASSERT_NE(mapped, nullptr) << GetLastError();
  nlohmann::json pending = Pending();
  pending["boot_attempts"] = 1;
  SetPending(pending);
  const Start beside = RunStart();
  FreeLibrary(mapped);
  std::vector<nlohmann::json> beside_trials;
  for (const nlohmann::json& record : beside.records) {
    if (record.value("event", "") == "update_trial") beside_trials.push_back(record);
  }
  ASSERT_EQ(beside_trials.size(), 1u);
  EXPECT_EQ(beside_trials[0].value("outcome", ""), "not_counted");
  EXPECT_EQ(beside_trials[0].value("reason", ""), "runtime_in_use");
  EXPECT_EQ(beside_trials[0].value("boot_attempts", 0), 1);
}

TEST_F(HostUpdateRollback, TheStartThatFinishesASwapsCleanupSaysItIsNotATry) {
  SwappedIn();
  Put(L"update\\swap.json", Journal("done"));
  const Start start = RunStart();
  std::vector<nlohmann::json> trials;
  for (const nlohmann::json& record : start.records) {
    if (record.value("event", "") == "update_trial") trials.push_back(record);
  }
  ASSERT_EQ(trials.size(), 1u);
  EXPECT_EQ(trials[0].value("outcome", ""), "not_counted");
  EXPECT_EQ(trials[0].value("reason", ""), "swap_finished");
  EXPECT_EQ(Pending().value("boot_attempts", 0), 1);
}

// The mark of a failure that cannot be moved reports whether it was made (review finding 4).
TEST_F(HostUpdateRollback, AMarkThatCannotBeWrittenWhileTheRuntimeIsInUseIsReportedAsNotMade) {
  PendingReleaseThatTheVerifierRefuses();
  const HMODULE mapped = LoadLibraryExW((update_ + L"\\previous\\7.7.7\\nevr-runtime.dll").c_str(), nullptr, 0);
  ASSERT_NE(mapped, nullptr);
  Put(L"nevr-runtime.dll", bad_runtime_);
  // The pending runtime is mapped by "another process" (this one): only a real image mapping refuses the write-open, so
  // map the unsigned stand-in itself.
  FreeLibrary(mapped);
  const HMODULE runtime = LoadLibraryExW(runtime_path_.c_str(), nullptr, 0);
  if (runtime == nullptr) GTEST_SKIP() << "the unsigned stand-in cannot be mapped: " << GetLastError();
  Held held(update_ + L"\\pending.json", FILE_SHARE_READ);  // and the record cannot be rewritten while it is held
  ASSERT_TRUE(held.ok());
  BootstrapDiagnostics diagnostics;
  const std::wstring host_dir = scratch_ + L"\\";
  diagnostics.PrepareForTest(local_.c_str(), host_dir.c_str());
  {
    std::lock_guard<std::mutex> lock(g_records_mutex);
    g_records.clear();
  }
  UpdateLock lock;
  const UpdateResult stop = nevr_host::RollBackInProcess(runtime_path_.c_str(), diagnostics, lock, "runtime_refused");
  FreeLibrary(runtime);
  EXPECT_EQ(stop.action, UpdateAction::kStop);
  std::lock_guard<std::mutex> guard(g_records_mutex);
  bool saw = false;
  for (const nlohmann::json& record : RollbackRecords(g_records)) {
    if (record.value("reason", "") == "runtime_in_use") {
      saw = true;
      EXPECT_TRUE(record.contains("marked")) << record.dump();
      EXPECT_FALSE(record.value("marked", true)) << record.dump();
    }
  }
  EXPECT_TRUE(saw);
}

// ---- refusals that must fire (review finding 2) ------------------------------------------------------------------------

class HostUpdateRollbackRefusals : public HostUpdateRollback {
 protected:
  std::vector<nlohmann::json> RecordsOfRollback(const Start& start) const { return RollbackRecords(start.records); }

  // Runs one start of a root that needs a rollback and returns it; the root is as SwappedIn + MarkCrashed left it, with
  // `setup` applied in between.
  Start CrashedStartAfter(const std::function<void()>& setup) {
    SwappedIn();
    MarkCrashed();
    setup();
    return RunStart();
  }

  nlohmann::json OnlyRollbackRecord(const Start& start) {
    const auto records = RollbackRecords(start.records);
    EXPECT_EQ(records.size(), 1u);
    return records.empty() ? nlohmann::json::object() : records[0];
  }
};

// U1: a link where the rollback would move files to or from: nothing moves through it, and the pending release loads.
TEST_F(HostUpdateRollbackRefusals, AFolderTheRollbackWorksInThatIsALinkRefusesTheRollbackAndMovesNothing) {
  struct Case {
    const char* label;
    const wchar_t* link;
    bool make_real_first;
    const char* folder;
  };
  const Case cases[] = {{"previous version", L"update\\previous\\7.7.7", false, "update/previous/7.7.7"},
                        {"previous plugins", L"update\\previous\\7.7.7\\plugins", false, "update/previous/7.7.7/plugins"},
                        {"failed version", L"update\\failed\\1.2.3", true, "update/failed/1.2.3"},
                        {"failed plugins", L"update\\failed\\1.2.3\\plugins", true, "update/failed/1.2.3/plugins"}};
  for (const Case& c : cases) {
    SCOPED_TRACE(c.label);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    SwappedIn();
    MarkCrashed();
    if (c.make_real_first) {
      Put(L"update\\failed\\1.2.3\\plugins\\keep.txt", "an earlier failed release");
    }
    ASSERT_FALSE(LinkAway(c.link).empty());
    const auto before = Tree(root_);
    const Start start = RunStart();
    EXPECT_EQ(start.result.action, UpdateAction::kLoad);
    EXPECT_TRUE(start.result.pending) << "the pending release loads as it is";
    EXPECT_EQ(Tree(root_), before) << "nothing moved through the link";
    const nlohmann::json record = OnlyRollbackRecord(start);
    EXPECT_EQ(record.value("outcome", ""), "failed");
    EXPECT_EQ(record.value("reason", ""), "reparse_point");
    EXPECT_EQ(record.value("folder", ""), c.folder);
  }
}

// U2: a file that is not the failed release's where a restored plugin goes is not replaced.
TEST_F(HostUpdateRollbackRefusals, AStrangersFileWhereARestoredPluginGoesRefusesTheRollbackAndMovesNothing) {
  const Start start = CrashedStartAfter([this] { Put(L"plugins\\example.dll", "a plugin a player put over the release's"); });
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.result.pending);
  EXPECT_EQ(Get(L"plugins\\example.dll"), "a plugin a player put over the release's");
  EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_);
  EXPECT_EQ(Get(L"update\\previous\\7.7.7\\plugins\\example.dll"), old_plugin_);
  const nlohmann::json record = OnlyRollbackRecord(start);
  EXPECT_EQ(record.value("outcome", ""), "failed");
  EXPECT_EQ(record.value("reason", ""), "foreign_file");
  EXPECT_EQ(record.value("path", ""), "plugins/example.dll");
}

// U3: an interrupted rollback whose folders became links is not completed through them; at done every file is in place.
TEST_F(HostUpdateRollbackRefusals, AnInterruptedRollbackWhoseFolderBecameALinkIsNotCompletedThroughIt) {
  for (const char* phase : {"out", "in", "done"}) {
    SCOPED_TRACE(phase);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    SwappedIn();
    nlohmann::json journal = {{"kind", "rollback"}, {"from", "1.2.3"}, {"to", "7.7.7"}, {"phase", phase},
                              {"out", {"nevr-runtime.dll", "plugins/example.dll"}},
                              {"in", {"nevr-runtime.dll", "plugins/example.dll"}},
                              {"failed_sha256", Sha256Hex(new_runtime_)}, {"reason", "crashed"}};
    Put(L"update\\swap.json", journal.dump());
    ASSERT_FALSE(LinkAway(L"update\\previous\\7.7.7").empty());
    const auto before = Tree(root_);
    const Start start = RunStart();
    if (std::string(phase) == "done") {
      EXPECT_EQ(start.result.action, UpdateAction::kLoad);
    } else {
      EXPECT_EQ(start.result.action, UpdateAction::kStop);
      ASSERT_NE(start.result.reason, nullptr);
      EXPECT_STREQ(start.result.reason, "reparse_point");
    }
    EXPECT_EQ(Tree(root_), before) << "nothing moved through the link";
    bool blocked = false;
    for (const nlohmann::json& record : SwapRecords(start.records)) {
      if (record.value("outcome", "") == "blocked" && record.value("reason", "") == "reparse_point") {
        blocked = true;
        EXPECT_EQ(record.value("kind", ""), "rollback");
        EXPECT_EQ(record.value("folder", ""), "update/previous/7.7.7");
      }
    }
    EXPECT_TRUE(blocked);
  }
}

TEST_F(HostUpdateRollbackRefusals, AJournalThatCannotBeWrittenRefusesTheRollbackAndLeavesTheRootAsItWas) {
  SwappedIn();
  MarkCrashed();
  // A directory where the journal's temporary file goes: the write fails with an error, not silently.
  MakeDirs(update_ + L"\\swap.json.tmp-" + std::to_wstring(GetCurrentProcessId()));
  const auto before = Tree(root_);
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.result.pending);
  EXPECT_EQ(Tree(root_), before);
  const nlohmann::json record = OnlyRollbackRecord(start);
  EXPECT_EQ(record.value("outcome", ""), "failed");
  EXPECT_EQ(record.value("reason", ""), "journal_write_failed");
  EXPECT_GT(record.value("win32_error", 0u), 0u);
}

TEST_F(HostUpdateRollbackRefusals, ACountThatCannotBeWrittenIsRecordedWithTheErrorAndTheReleaseStillLoads) {
  SwappedIn();
  MakeDirs(update_ + L"\\pending.json.tmp-" + std::to_wstring(GetCurrentProcessId()));
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.result.pending);
  EXPECT_EQ(Pending().value("boot_attempts", 0), 1) << "the count was not written";
  bool saw = false;
  for (const nlohmann::json& record : RollbackRecords(start.records)) {
    if (record.value("reason", "") == "pending_write_failed") {
      saw = true;
      EXPECT_EQ(record.value("outcome", ""), "blocked");
      EXPECT_GT(record.value("win32_error", 0u), 0u);
    }
  }
  EXPECT_TRUE(saw);
  // The start also says it was not a try, and why.
  bool trial = false;
  for (const nlohmann::json& record : start.records) {
    if (record.value("event", "") == "update_trial") {
      trial = true;
      EXPECT_EQ(record.value("outcome", ""), "not_counted") << record.dump();
      EXPECT_EQ(record.value("reason", ""), "pending_write_failed") << record.dump();
      EXPECT_GT(record.value("win32_error", 0u), 0u) << record.dump();
      EXPECT_EQ(record.value("boot_attempts", 0), 1) << record.dump();
    }
  }
  EXPECT_TRUE(trial) << "a count that could not be written is reported as a trial that was not counted";
}

// A start whose pending release needs no rollback, with a runtime the host cannot open for write, is not a try either, and
// says why.
TEST_F(HostUpdateRollbackRefusals, AStartWhoseRuntimeCannotBeOpenedForWriteIsNotCountedAndSaysWhy) {
  SwappedIn();
  ASSERT_NE(SetFileAttributesW(runtime_path_.c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
  const Start start = RunStart();
  SetFileAttributesW(runtime_path_.c_str(), FILE_ATTRIBUTE_NORMAL);
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.result.pending);
  EXPECT_EQ(Pending().value("boot_attempts", 0), 1) << "not counted";
  std::vector<nlohmann::json> trials;
  for (const nlohmann::json& record : start.records) {
    if (record.value("event", "") == "update_trial") trials.push_back(record);
  }
  ASSERT_EQ(trials.size(), 1u);
  EXPECT_EQ(trials[0].value("outcome", ""), "not_counted");
  EXPECT_EQ(trials[0].value("reason", ""), "runtime_not_writable");
  EXPECT_GT(trials[0].value("win32_error", 0u), 0u);
  EXPECT_EQ(trials[0].value("boot_attempts", 0), 1);
}

// The mark of a failure that stays in the root cannot be written: the in-process rollback says so (the stop is the same).
TEST_F(HostUpdateRollbackRefusals, AnInProcessStopWhoseMarkCannotBeWrittenSaysSo) {
  PendingReleaseThatTheVerifierRefuses();
  DeleteFileW((update_ + L"\\previous\\7.7.7\\nevr-runtime.dll").c_str());
  MakeDirs(update_ + L"\\pending.json.tmp-" + std::to_wstring(GetCurrentProcessId()));  // every write of pending.json fails
  const LoadedStart run = LoadAtStart(ValidContext());
  ASSERT_EQ(run.result.outcome, nevr_host::RuntimeLoadOutcome::kUpdateBlocked);
  ASSERT_NE(run.result.update_reason, nullptr);
  EXPECT_STREQ(run.result.update_reason, "no_previous");
  EXPECT_TRUE(Pending()["failed_reason"].is_null()) << "the mark was not made";
  bool saw = false;
  for (const nlohmann::json& record : RollbackRecords(run.records)) {
    if (record.value("mode", "") == "in_process" && record.value("reason", "") == "pending_write_failed") {
      saw = true;
      EXPECT_EQ(record.value("outcome", ""), "failed");
      EXPECT_GT(record.value("win32_error", 0u), 0u);
    }
  }
  EXPECT_TRUE(saw) << "the failed mark is in the log";
}

TEST_F(HostUpdateRollbackRefusals, ARuntimeTheHostCannotOpenForWriteRefusesTheRollbackWithTheError) {
  SwappedIn();
  MarkCrashed();
  ASSERT_NE(SetFileAttributesW(runtime_path_.c_str(), FILE_ATTRIBUTE_READONLY), FALSE);
  const auto before = Tree(root_);
  const Start start = RunStart();
  SetFileAttributesW(runtime_path_.c_str(), FILE_ATTRIBUTE_NORMAL);
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.result.pending);
  EXPECT_EQ(Tree(root_), before);
  const nlohmann::json record = OnlyRollbackRecord(start);
  EXPECT_EQ(record.value("outcome", ""), "blocked");
  EXPECT_EQ(record.value("reason", ""), "runtime_not_writable");
  EXPECT_EQ(record.value("cause", ""), "crashed");
  EXPECT_GT(record.value("win32_error", 0u), 0u);
}

// A previous file another program holds open cannot be read for the journal: nothing moves, and the pending release loads.
TEST_F(HostUpdateRollbackRefusals, APreviousFileTheHostCannotReadRefusesTheRollbackBeforeAnythingMoves) {
  SwappedIn();
  MarkCrashed();
  auto before = Tree(root_);
  before.erase("update\\previous\\7.7.7\\plugins\\example.dll");  // the comparison cannot read the held file either
  Start start;
  {
    Held held(update_ + L"\\previous\\7.7.7\\plugins\\example.dll", 0);
    ASSERT_TRUE(held.ok());
    start = RunStart();
    auto after = Tree(root_);
    after.erase("update\\previous\\7.7.7\\plugins\\example.dll");
    EXPECT_EQ(after, before) << "nothing moved";
  }
  EXPECT_EQ(start.result.action, UpdateAction::kLoad);
  EXPECT_TRUE(start.result.pending);
  const nlohmann::json record = OnlyRollbackRecord(start);
  EXPECT_EQ(record.value("outcome", ""), "failed");
  EXPECT_EQ(record.value("reason", ""), "previous_unreadable");
  EXPECT_EQ(record.value("path", ""), "plugins/example.dll");
  // Released, the next start rolls back.
  RunStart();
  ExpectRolledBack();
}

// A cleanup from a journal that carries no sha256 (an older one) reads the files, and a file it cannot read leaves the
// cleanup for the next start: it writes no pending record that says "no previous" or names no release.
TEST_F(HostUpdateRollbackRefusals, ACleanupFromAJournalWithoutShasDoesNotWritePendingFromAFileItCannotRead) {
  SwappedIn();
  DeleteFileW((update_ + L"\\pending.json").c_str());
  Put(L"update\\swap.json", Journal("done"));
  {
    Held held(update_ + L"\\previous\\7.7.7\\nevr-runtime.dll", 0);
    ASSERT_TRUE(held.ok());
    RunStart();
    EXPECT_FALSE(Exists(update_ + L"\\pending.json")) << "not written from a previous runtime that only looked missing";
    EXPECT_TRUE(Exists(update_ + L"\\swap.json")) << "the next start finishes it";
  }
  RunStart();
  EXPECT_FALSE(Exists(update_ + L"\\swap.json"));
  EXPECT_TRUE(Pending()["from"].is_object()) << Pending().dump();
  EXPECT_EQ(Pending()["to"].value("sha256", ""), Sha256Hex(new_runtime_));
}

TEST_F(HostUpdateRollbackRefusals, ACleanupFromAJournalWithoutShasWritesNoPendingRecordForARuntimeThatIsNotThere) {
  SwappedIn();
  DeleteFileW((update_ + L"\\pending.json").c_str());
  DeleteFileW(runtime_path_.c_str());
  Put(L"update\\swap.json", Journal("done"));
  RunStart();
  EXPECT_FALSE(Exists(update_ + L"\\pending.json")) << "a record that cannot name its release is not written";
  EXPECT_TRUE(Exists(update_ + L"\\swap.json"));
}

TEST_F(HostUpdateRollbackRefusals, ALockAnotherStartHoldsStopsTheInProcessRollbackAndTheMarkWithTheCause) {
  PendingReleaseThatTheVerifierRefuses();
  nevr_host::SetUpdateLockTimingForTest(60, 10);
  UpdateLock other;
  ASSERT_TRUE(other.Acquire(update_.c_str(), 1000, 10));
  BootstrapDiagnostics diagnostics;
  const std::wstring host_dir = scratch_ + L"\\";
  diagnostics.PrepareForTest(local_.c_str(), host_dir.c_str());
  {
    std::lock_guard<std::mutex> guard(g_records_mutex);
    g_records.clear();
  }
  UpdateLock mine;
  const UpdateResult stop = nevr_host::RollBackInProcess(runtime_path_.c_str(), diagnostics, mine, "runtime_refused");
  EXPECT_EQ(stop.action, UpdateAction::kStop);
  ASSERT_NE(stop.reason, nullptr);
  EXPECT_STREQ(stop.reason, "lock_timeout");
  EXPECT_FALSE(nevr_host::MarkPendingFailed(runtime_path_.c_str(), diagnostics, mine, "init_failed"));
  std::lock_guard<std::mutex> guard(g_records_mutex);
  int timeouts = 0;
  for (const nlohmann::json& record : RollbackRecords(g_records)) {
    if (record.value("reason", "") == "lock_timeout") ++timeouts;
  }
  EXPECT_EQ(timeouts, 2) << "both say why they gave up";
  EXPECT_TRUE(Pending()["failed_reason"].is_null()) << "nothing was written without the lock";
}

TEST_F(HostUpdateRollbackRefusals, AnInProcessRollbackThatCannotReadTheStateOrThePendingRecordStopsWithThatCause) {
  for (const char* what : {"state.json", "pending.json"}) {
    SCOPED_TRACE(what);
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    PendingReleaseThatTheVerifierRefuses();
    BootstrapDiagnostics diagnostics;
    const std::wstring host_dir = scratch_ + L"\\";
    diagnostics.PrepareForTest(local_.c_str(), host_dir.c_str());
    Held held(update_ + L"\\" + Widen(what), 0);
    ASSERT_TRUE(held.ok());
    UpdateLock lock;
    const UpdateResult stop = nevr_host::RollBackInProcess(runtime_path_.c_str(), diagnostics, lock, "runtime_refused");
    EXPECT_EQ(stop.action, UpdateAction::kStop);
    ASSERT_NE(stop.reason, nullptr);
    EXPECT_STREQ(stop.reason, std::string(what) == "state.json" ? "state_unreadable" : "pending_unreadable");
    held.Close();
    EXPECT_EQ(Get(L"nevr-runtime.dll"), bad_runtime_) << "nothing moved";
  }
}

}  // namespace
