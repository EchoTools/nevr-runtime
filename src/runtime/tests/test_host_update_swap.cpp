// Self-update's swap at start (docs/adr/0024-pc-self-update.md, section 4; #2104), over real files in a temporary nevr
// root, under Wine. The release files are the signed fixtures tools/gen_plugin_signature_fixtures.py makes into the
// directory NEVR_SIGFIX_DIR names (`just test-auth-unit` makes it): `nevr-runtime.signed` (the rendezvous runtime
// stand-in, signed by the throwaway PKI, NEVR-BUILD 7.7.7) is the new runtime, `nevr-runtime.unsigned` (the same
// stand-in, unsigned) the one in the root, and `good` and `good2` two signed plugins. The tests fail without the
// directory: a swap test that skips is a swap nobody tested.
//
// Each test names the ADR 0024 failure case it covers (F6 to F23).

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
using nevr_host::UpdateStartAction;
using nevr_host::UpdateStartResult;
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
  UpdateStartResult result;
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

  // The usual start: the old release in the root (runtime and one owned plugin), the new one staged.
  void OldReleaseInTheRootAndANewOneStaged() {
    Put(L"nevr-runtime.dll", old_runtime_);
    Put(L"plugins\\example.dll", old_plugin_);
    WriteState({{"channel", "beta"}, {"owned", {{{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(old_plugin_)}}}}});
    Stage("1.2.3", {{"nevr-runtime.dll", new_runtime_}, {"plugins/example.dll", new_plugin_}});
  }

  // What the root holds after that release was swapped in.
  void ExpectSwapped() {
    EXPECT_EQ(Get(L"nevr-runtime.dll"), new_runtime_) << "the staged runtime is in the root";
    EXPECT_EQ(Get(L"plugins\\example.dll"), new_plugin_) << "the staged plugin is in the root";
    EXPECT_EQ(Get(L"update\\previous\\7.7.7\\nevr-runtime.dll"), old_runtime_) << "the old runtime is the previous one";
    EXPECT_EQ(Get(L"update\\previous\\7.7.7\\plugins\\example.dll"), old_plugin_);
    EXPECT_FALSE(Exists(update_ + L"\\staged.json"));
    EXPECT_FALSE(Exists(update_ + L"\\swap.json"));
    EXPECT_FALSE(Exists(update_ + L"\\staging\\1.2.3"));
    const nlohmann::json pending = ReadJson(update_ + L"\\pending.json");
    EXPECT_EQ(pending, nlohmann::json({{"to", {{"version", "1.2.3"}, {"sha256", Sha256Hex(new_runtime_)}}},
                                       {"from", {{"version", "7.7.7"}, {"sha256", Sha256Hex(old_runtime_)}}},
                                       {"boot_attempts", 1},
                                       {"failed_reason", nullptr}}))
        << pending.dump();
    const nlohmann::json state = ReadJson(update_ + L"\\state.json");
    EXPECT_EQ(state.value("channel", ""), "beta") << state.dump();
    EXPECT_EQ(state["owned"], nlohmann::json::array({{{"path", "nevr-runtime.dll"}, {"sha256", Sha256Hex(new_runtime_)}},
                                                     {{"path", "plugins/example.dll"}, {"sha256", Sha256Hex(new_plugin_)}}}))
        << state.dump();
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
  EXPECT_EQ(start.result.action, UpdateStartAction::kNoUpdateFolder);
  EXPECT_FALSE(start.lock_held);
  EXPECT_TRUE(start.records.empty());
  EXPECT_EQ(Tree(scratch_), before) << "nothing was written anywhere";
}

TEST_F(HostUpdateSwap, AnUpdateFolderWithNothingStagedTakesTheLockAndChangesNothing) {
  Put(L"nevr-runtime.dll", old_runtime_);
  MakeDirs(update_);
  const auto before = Tree(root_);
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateStartAction::kLoad);
  EXPECT_TRUE(start.lock_held) << "the lock is held until the runtime is mapped";
  EXPECT_TRUE(SwapRecords(start.records).empty());
  EXPECT_EQ(Tree(root_), before);
}

TEST_F(HostUpdateSwap, AVerifiedStagedReleaseIsSwappedInAndTheOldOneIsKeptAsPrevious) {
  OldReleaseInTheRootAndANewOneStaged();
  Put(L"update\\previous\\0.0.1\\nevr-runtime.dll", "an older generation");
  const Start start = RunStart();
  EXPECT_EQ(start.result.action, UpdateStartAction::kLoad);
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
  EXPECT_EQ(start.result.action, UpdateStartAction::kLoad);
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
    const Start next = RunStart();
    EXPECT_EQ(next.result.action, UpdateStartAction::kLoad);
    ExpectSwapped();
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

// F8: a dedicated server beside a client shares the root; the swap waits until nobody runs the runtime.
TEST_F(HostUpdateSwap, F8_ARuntimeAnotherProcessHasMappedDefersTheSwap) {
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
  EXPECT_EQ(Tree(root_), before) << "staged.json stays for a later start";
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
  EXPECT_EQ(start.result.action, UpdateStartAction::kLoad);
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
  EXPECT_EQ(start.result.action, UpdateStartAction::kStop);
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
  EXPECT_EQ(results[0].result.action, UpdateStartAction::kLoad);
  EXPECT_EQ(results[1].result.action, UpdateStartAction::kLoad);
  ExpectSwapped();
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
  const std::vector<Case> cases = {
      {"version ..", staged("..", "nevr-runtime.dll")},
      {"version CON", staged("CON", "nevr-runtime.dll")},
      {"version with a backslash", staged("a\\b", "nevr-runtime.dll")},
      {"path ../", staged("1.2.3", "../nevr-runtime.dll")},
      {"path plugins/../", staged("1.2.3", "plugins/../nevr-runtime.dll")},
      {"path with a backslash", staged("1.2.3", "plugins\\x.dll")},
      {"device plugin", staged("1.2.3", "plugins/CON.dll")},
      {"no runtime", staged("1.2.3", "plugins/example.dll")},
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
    EXPECT_EQ(start.result.action, UpdateStartAction::kLoad);
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
    EXPECT_EQ(start.result.action, UpdateStartAction::kStop);
    ASSERT_NE(start.result.reason, nullptr);
    EXPECT_STREQ(start.result.reason, "bad_journal");
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "blocked");
    EXPECT_EQ(records[0].value("reason", ""), "bad_journal");
    EXPECT_EQ(Tree(root_), before);
  }
}

TEST_F(HostUpdateSwap, F22_AReparsePointInTheUpdateFoldersBlocksTheSwap) {
  for (const wchar_t* linked : {L"update\\staging", L"plugins"}) {
    SCOPED_TRACE(Narrow(linked));
    RemoveTree(root_);
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    OldReleaseInTheRootAndANewOneStaged();
    // Move the real folder aside and put a directory link where it was.
    const std::wstring path = root_ + L"\\" + linked;
    const std::wstring elsewhere = scratch_ + L"\\elsewhere-" + std::to_wstring(GetTickCount64());
    ASSERT_NE(MoveFileExW(path.c_str(), elsewhere.c_str(), 0), FALSE) << GetLastError();
    ASSERT_NE(CreateSymbolicLinkW(path.c_str(), elsewhere.c_str(),
                                  SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE),
              FALSE)
        << GetLastError();
    ASSERT_NE(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT, 0u)
        << "this system does not report the link as a reparse point";
    const std::string before_runtime = Get(L"nevr-runtime.dll");
    const Start start = RunStart();
    const auto records = SwapRecords(start.records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].value("outcome", ""), "blocked");
    EXPECT_EQ(records[0].value("reason", ""), "reparse_point");
    EXPECT_EQ(Get(L"nevr-runtime.dll"), before_runtime);
    EXPECT_TRUE(Exists(update_ + L"\\staged.json"));
    RemoveDirectoryW(path.c_str());
    RemoveTree(elsewhere);
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
    EXPECT_EQ(start.result.action, UpdateStartAction::kLoad);
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

}  // namespace
