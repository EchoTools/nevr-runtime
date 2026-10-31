// The host's verdict on nevr-runtime.dll before it maps the file (docs/adr/0012-static-install-root.md, "The host
// verifies nevr-runtime.dll before it loads it"; #854).
//
// Three layers, each with the outcomes the issue names (missing, unsigned, tampered, wrong signer, valid, and an
// unsigned file in a dev build):
//   1. DecideVerify, the pure decision: every row of its table.
//   2. The detour on the game's main wrapper over a real file staged next to a host stand-in, through the production
//      RuntimeRendezvous / RuntimeLoader / BootstrapDiagnostics: the exit code, the one dialog, the `versions` record,
//      the order of the records, and whether the file was ever mapped.
//   3. The words the player reads for each cause.
//
// The signed files are the rendezvous runtime stand-in, signed by tools/gen_plugin_signature_fixtures.py with a
// throwaway PKI into the directory named by NEVR_SIGFIX_DIR (`just test-auth-unit` makes it). The tests that need them
// fail without it: a verifier test that skips is a verifier nobody tested.

#include "runtime/host/host_identity.h"
#include "runtime/host/runtime_load.h"
#include "runtime/host/runtime_rendezvous.h"
#include "runtime/host/stop_line.h"
#include "runtime/host/stop_notice.h"
#include "runtime/tests/stderr_capture.h"

#include <gtest/gtest.h>

#include <MinHook.h>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <wincrypt.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using nevr_host::BootstrapDiagnostics;
using nevr_host::DecideVerify;
using nevr_host::RuntimeFile;
using nevr_host::RuntimeLoadOutcome;
using nevr_host::RuntimeRendezvous;
using nevr_host::StopNoticeTextFor;
using nevr_plugin_signature::Reason;

// ---- 1. the decision -------------------------------------------------------------------------------------------

struct Row {
  RuntimeFile file;
  bool verify_enabled;
  bool accepted;
  bool dev_build;
  bool refused;
  bool may_load;
  const char* verify;
  bool reason_is_verdict;
};

TEST(HostVerifyDecision, EveryRowOfTheTable) {
  const Row rows[] = {
      // An absent file is a load that fails by itself: nothing was checked, so nothing is said about a check.
      {RuntimeFile::kAbsent, true, false, false, false, false, nullptr, false},
      {RuntimeFile::kAbsent, true, true, true, false, false, nullptr, false},
      // A file that cannot be opened for reading is refused in every build: a dev build allows an unsigned file, not
      // one it cannot read.
      {RuntimeFile::kUnreadable, true, false, false, true, false, "refused", false},
      {RuntimeFile::kUnreadable, true, false, true, true, false, "refused", false},
      {RuntimeFile::kUnreadable, true, true, true, true, false, "refused", false},
      // A test harness's fixture runtime is loaded without a check, and the record says so.
      {RuntimeFile::kReadable, false, false, false, false, true, "not_checked", false},
      // The production rows: accepted loads, an unaccepted file loads only in a dev build.
      {RuntimeFile::kReadable, true, true, false, false, true, "accepted", false},
      {RuntimeFile::kReadable, true, true, true, false, true, "accepted", false},
      {RuntimeFile::kReadable, true, false, true, false, true, "unsigned_allowed", true},
      {RuntimeFile::kReadable, true, false, false, true, false, "refused", true},
  };
  int index = 0;
  for (const Row& row : rows) {
    SCOPED_TRACE(index++);
    const nevr_host::VerifyDecision decision =
        DecideVerify(row.file, row.verify_enabled, row.accepted, row.dev_build);
    EXPECT_EQ(decision.refused, row.refused);
    EXPECT_EQ(decision.may_load, row.may_load);
    if (row.verify == nullptr) {
      EXPECT_EQ(decision.verify, nullptr);
    } else {
      ASSERT_NE(decision.verify, nullptr);
      EXPECT_STREQ(decision.verify, row.verify);
    }
    EXPECT_EQ(decision.reason_is_verdict, row.reason_is_verdict);
  }
}

TEST(HostVerifyDecision, ARefusedFileIsNeverLoadableWhateverTheOtherInputsAre) {
  for (const RuntimeFile file : {RuntimeFile::kAbsent, RuntimeFile::kUnreadable, RuntimeFile::kReadable}) {
    for (const bool enabled : {false, true}) {
      for (const bool accepted : {false, true}) {
        for (const bool dev : {false, true}) {
          const nevr_host::VerifyDecision decision = DecideVerify(file, enabled, accepted, dev);
          EXPECT_FALSE(decision.refused && decision.may_load);
        }
      }
    }
  }
}

// ---- 3. the words ----------------------------------------------------------------------------------------------

TEST(HostStopNoticeText, EachCauseSaysWhatIsWrongAndWhatToDo) {
  const std::wstring missing = StopNoticeTextFor(RuntimeLoadOutcome::kRuntimeAbsent, Reason::Accepted);
  EXPECT_NE(missing.find(L"nevr-runtime.dll is missing"), std::wstring::npos) << "a missing file says it is missing";
  EXPECT_NE(missing.find(L"installer"), std::wstring::npos);

  const std::wstring unsigned_text = StopNoticeTextFor(RuntimeLoadOutcome::kRuntimeRefused, Reason::Unsigned);
  EXPECT_NE(unsigned_text.find(L"not signed"), std::wstring::npos);
  const std::wstring changed = StopNoticeTextFor(RuntimeLoadOutcome::kRuntimeRefused, Reason::DigestMismatch);
  EXPECT_NE(changed.find(L"changed after it was signed"), std::wstring::npos);
  EXPECT_EQ(changed, StopNoticeTextFor(RuntimeLoadOutcome::kRuntimeRefused, Reason::SignatureInvalid));
  const std::wstring unreadable = StopNoticeTextFor(RuntimeLoadOutcome::kRuntimeRefused, Reason::Unreadable);
  EXPECT_NE(unreadable.find(L"could not be read"), std::wstring::npos);
  const std::wstring other = StopNoticeTextFor(RuntimeLoadOutcome::kRuntimeRefused, Reason::MissingEku);
  EXPECT_NE(other.find(L"signature"), std::wstring::npos);

  for (const std::wstring* text : {&missing, &unsigned_text, &changed, &unreadable, &other}) {
    EXPECT_NE(text->find(L"nevr-runtime.dll"), std::wstring::npos) << "every notice names the file";
  }
  EXPECT_NE(missing, unsigned_text);
  EXPECT_NE(unsigned_text, changed);
  EXPECT_NE(changed, unreadable);
  EXPECT_NE(unreadable, other);

  // Every other way the runtime can fail to start keeps the general words.
  for (const RuntimeLoadOutcome outcome :
       {RuntimeLoadOutcome::kNoPath, RuntimeLoadOutcome::kDependencyLoadFailed, RuntimeLoadOutcome::kLoadFailed,
        RuntimeLoadOutcome::kMissingEntry, RuntimeLoadOutcome::kNullDescriptor, RuntimeLoadOutcome::kAbiRejected,
        RuntimeLoadOutcome::kInitFailed}) {
    EXPECT_EQ(StopNoticeTextFor(outcome, Reason::Accepted), std::wstring(nevr_host::kStopNoticeText));
  }
}

// ---- 2. the detour over a staged file --------------------------------------------------------------------------

std::vector<std::string> g_events;
std::vector<std::wstring> g_notices;
std::uint32_t g_stop_code = 0;
const char* g_command_line = "";
std::atomic<int> g_before_load_calls{0};
DWORD g_write_open_error = 0;
bool g_write_open_succeeded = false;
bool g_delete_succeeded = false;
std::wstring g_log_path;
bool g_versions_written_before_load = false;

void RecordNotice(const wchar_t* text) {
  g_events.push_back("notice");
  g_notices.emplace_back(text);
}
void RecordStop(std::uint32_t code) {
  g_events.push_back("stop");
  g_stop_code = code;
}
const char* CommandLine() { return g_command_line; }

std::string ReadAll(const std::wstring& path);

// While the file is held between the verdict and the load, nothing may write, delete or replace it.
void BeforeLoad(const wchar_t* path) {
  g_before_load_calls.fetch_add(1);
  const HANDLE writer = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  g_write_open_succeeded = writer != INVALID_HANDLE_VALUE;
  g_write_open_error = g_write_open_succeeded ? 0 : GetLastError();
  if (g_write_open_succeeded) CloseHandle(writer);
  g_delete_succeeded = DeleteFileW(path) != FALSE;
  // The versions record is on disk before the runtime can run a line of its own.
  g_versions_written_before_load = ReadAll(g_log_path).find("\"event\":\"versions\"") != std::string::npos;
}

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

std::vector<unsigned char> ReadBytes(const std::wstring& path) {
  const std::string text = ReadAll(path);
  return {text.begin(), text.end()};
}

void WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE) << GetLastError();
  DWORD written = 0;
  EXPECT_TRUE(WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
  EXPECT_EQ(written, bytes.size());
  CloseHandle(file);
}

std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
  HCRYPTPROV provider = 0;
  HCRYPTHASH hash = 0;
  std::string hex;
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return hex;
  if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
      CryptHashData(hash, bytes.data(), static_cast<DWORD>(bytes.size()), 0)) {
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

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// A Unix path as Wine takes it from a Windows API.
std::wstring WinPath(const std::string& unix_path) {
  if (unix_path.empty() || unix_path[0] != '/') return Widen(unix_path);
  std::wstring out = L"Z:";
  for (const char c : unix_path) out.push_back(c == '/' ? L'\\' : static_cast<wchar_t>(c));
  return out;
}

// Executable memory that starts with the pinned prologue and returns 0x12345678 as a function that leaves its frame,
// so the trampoline MinHook builds can really be called (the runtime stand-in's game_main calls it).
class ReturningTarget {
 public:
  ReturningTarget() {
    code_ = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (code_ == nullptr) return;
    std::memcpy(code_, nevr_host::kGameMainWrapperPrologue, nevr_host::kGameMainWrapperPrologueSize);
    const unsigned char tail[] = {0xB8, 0x78, 0x56, 0x34, 0x12, 0x48, 0x83, 0xC4, 0x38, 0xC3};
    std::memcpy(code_ + nevr_host::kGameMainWrapperPrologueSize, tail, sizeof(tail));
    std::memset(code_ + nevr_host::kGameMainWrapperPrologueSize + sizeof(tail), 0xCC, 32);
  }
  ~ReturningTarget() {
    MH_Uninitialize();
    if (code_ != nullptr) VirtualFree(code_, 0, MEM_RELEASE);
  }
  unsigned char* code() const { return code_; }

 private:
  unsigned char* code_ = nullptr;
};

std::vector<nlohmann::json> ParseRecords(const std::string& text) {
  std::vector<nlohmann::json> records;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) break;
    records.push_back(nlohmann::json::parse(text.substr(start, end - start)));
    start = end + 1;
  }
  return records;
}

int IndexOf(const std::vector<nlohmann::json>& records, const char* event) {
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (records[i].value("event", "") == event) return static_cast<int>(i);
  }
  return -1;
}

constexpr std::int64_t kOriginalResult = 0x12345678;

struct Outcome {
  std::int64_t result = 0;
  std::vector<nlohmann::json> records;
  bool runtime_mapped = false;
  std::string stderr_text;  // what the host wrote to the standard error handle (the NEVR.STOP line)
};

// The one NEVR.STOP line of a run that stopped the game, parsed; fails the test when there is not exactly one. A stop
// writes two lines to the standard error handle: the JSON line (#1252), then the plain notice line (#1533).
nlohmann::json OneStopLine(const Outcome& outcome) {
  EXPECT_FALSE(outcome.stderr_text.empty());
  const std::size_t first_end = outcome.stderr_text.find('\n');
  EXPECT_NE(first_end, std::string::npos) << outcome.stderr_text;
  if (first_end == std::string::npos) return nlohmann::json();
  const std::string notice = outcome.stderr_text.substr(first_end + 1);
  EXPECT_EQ(notice.rfind("nEVR couldn't start: ", 0), 0u) << "the second line is the plain notice: " << outcome.stderr_text;
  EXPECT_EQ(notice.find('\n'), notice.size() - 1)
      << "two lines in all, however often the detour runs: " << outcome.stderr_text;
  return nlohmann::json::parse(outcome.stderr_text.substr(0, first_end));
}

std::string Utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
  return out;
}

// The one record of an event, which must be there exactly once.
nlohmann::json Only(const Outcome& outcome, const char* event) {
  nlohmann::json found;
  int count = 0;
  for (const nlohmann::json& record : outcome.records) {
    if (record.value("event", "") == event) {
      found = record;
      ++count;
    }
  }
  EXPECT_EQ(count, 1) << event;
  return found;
}

class HostRuntimeVerify : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* dir = std::getenv("NEVR_SIGFIX_DIR");
    ASSERT_NE(dir, nullptr) << "NEVR_SIGFIX_DIR is not set: run tools/gen_plugin_signature_fixtures.py <dir> "
                               "--plugin-dll <the rendezvous nevr-runtime.dll> and export it (just test-auth-unit does)";
    fixtures_ = dir;
    std::ifstream in(fixtures_ + "/fixtures.json");
    ASSERT_TRUE(in.good()) << "no fixtures.json in " << fixtures_;
    manifest_ = nlohmann::json::parse(in);
    ASSERT_TRUE(manifest_["files"].contains("nevr-runtime.signed"))
        << "the fixtures hold no signed nevr-runtime stand-in: pass --plugin-dll <rendezvous nevr-runtime.dll>";
    const std::vector<unsigned char> root = ReadBytes(WinPath(fixtures_ + "/" + manifest_["root_der"].get<std::string>()));
    ASSERT_FALSE(root.empty());
    policy_.rootDer.assign(root.begin(), root.end());
    policy_.profileEkus = {manifest_["profile_eku"].get<std::string>()};

    wchar_t base[MAX_PATH] = {};
    ASSERT_GT(GetTempPathW(MAX_PATH, base), 0u);
    static LONG counter = 0;
    wchar_t dir_name[MAX_PATH] = {};
    swprintf(dir_name, MAX_PATH, L"%ls\\nevr-verify-%lu-%llu-%ld", base, static_cast<unsigned long>(GetCurrentProcessId()),
             static_cast<unsigned long long>(GetTickCount64()), static_cast<long>(InterlockedIncrement(&counter)));
    root_ = dir_name;
    ASSERT_NE(CreateDirectoryW(root_.c_str(), nullptr), FALSE);
    install_ = root_ + L"\\install";
    local_ = root_ + L"\\local";
    ASSERT_NE(CreateDirectoryW(install_.c_str(), nullptr), FALSE);
    ASSERT_NE(CreateDirectoryW(local_.c_str(), nullptr), FALSE);
    nevr_root_ = root_ + L"\\nevr-root";
    ASSERT_NE(CreateDirectoryW(nevr_root_.c_str(), nullptr), FALSE);
    runtime_path_ = nevr_root_ + L"\\nevr-runtime.dll";
    nevr_host::SetRuntimeDirForTest(nevr_root_.c_str());
    g_log_path = local_ + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl";
    g_versions_written_before_load = false;

    // The host stand-in, in the install directory: the game's folder, where the runtime is no longer looked for.
    // Any plain DLL will do. The runtime is looked for in nevr_root_ through the seam; the production rule is
    // the subject of test_host_runtime_load.
    const std::wstring host_path = install_ + L"\\BugSplat64.dll";
    ASSERT_TRUE(CopyFileW(WinPath(fixtures_ + "/" + manifest_["files"]["good"].get<std::string>()).c_str(),
                          host_path.c_str(), FALSE));
    host_ = LoadLibraryExW(host_path.c_str(), nullptr, 0);
    ASSERT_NE(host_, nullptr) << GetLastError();
    host_sha256_ = Sha256Hex(ReadBytes(host_path));

    g_events.clear();
    g_notices.clear();
    g_stop_code = 0;
    g_command_line = "echovr.exe -windowed";
    g_before_load_calls.store(0);
    g_write_open_error = 0;
    g_write_open_succeeded = false;
    g_delete_succeeded = false;
    nevr_host::SetVerifyRuntimeForTest(true);
    nevr_host::SetVerifyPolicyForTest(&policy_);
    nevr_host::SetDevBuildForTest(0);
    nevr_host::SetBeforeLoadHookForTest(&BeforeLoad);
  }

  void TearDown() override {
    nevr_host::SetVerifyRuntimeForTest(false);
    nevr_host::SetVerifyPolicyForTest(nullptr);
    nevr_host::SetDevBuildForTest(-1);
    nevr_host::SetBeforeLoadHookForTest(nullptr);
    nevr_host::SetRuntimeDirForTest(nullptr);
    RuntimeRendezvous::Seams() = RuntimeRendezvous::TestSeams{};
    if (host_ != nullptr) FreeLibrary(host_);
    // Best effort: the staged files of this test, by exact path.
    DeleteFileW(runtime_path_.c_str());
    DeleteFileW((install_ + L"\\nevr-runtime.dll").c_str());
    RemoveDirectoryW(nevr_root_.c_str());
    DeleteFileW((install_ + L"\\BugSplat64.dll").c_str());
    RemoveDirectoryW(install_.c_str());
    DeleteFileW((local_ + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  }

  std::wstring FixtureFile(const char* key) const {
    return WinPath(fixtures_ + "/" + manifest_["files"][key].get<std::string>());
  }

  void StageCopy(const char* key) { ASSERT_TRUE(CopyFileW(FixtureFile(key).c_str(), runtime_path_.c_str(), FALSE)); }

  // One byte of the first section's raw data changed: the file still has its signature and no longer hashes to it.
  void StageTampered() {
    std::vector<unsigned char> bytes = ReadBytes(FixtureFile("nevr-runtime.signed"));
    ASSERT_GT(bytes.size(), 0x400u);
    std::uint32_t pe = 0;
    std::memcpy(&pe, bytes.data() + 0x3c, sizeof(pe));
    std::uint16_t sections = 0;
    std::memcpy(&sections, bytes.data() + pe + 6, sizeof(sections));
    std::uint16_t optional_size = 0;
    std::memcpy(&optional_size, bytes.data() + pe + 20, sizeof(optional_size));
    ASSERT_GT(sections, 0);
    const std::size_t first_section = pe + 24 + optional_size;
    std::uint32_t raw_pointer = 0;
    std::memcpy(&raw_pointer, bytes.data() + first_section + 20, sizeof(raw_pointer));
    ASSERT_LT(raw_pointer + 16, bytes.size());
    bytes[raw_pointer + 8] ^= 0x01;
    WriteBytes(runtime_path_, bytes);
  }

  Outcome Run() {
    Outcome outcome;
    RuntimeRendezvous::Seams().stop_game = &RecordStop;
    RuntimeRendezvous::Seams().show_stop_notice = &RecordNotice;
    RuntimeRendezvous::Seams().command_line = &CommandLine;
    BootstrapDiagnostics diagnostics;
    // The host stand-in sits in install_: its directory is where an old-layout runtime would be found.
    const std::wstring host_dir = install_ + L"\\";
    diagnostics.PrepareForTest(local_.c_str(), nullptr, host_dir.c_str());
    nevr_test::StderrCapture capture;
    {
      ReturningTarget target;
      EXPECT_NE(target.code(), nullptr);
      RuntimeRendezvous rendezvous;
      rendezvous.Prepare(host_, GetModuleHandleW(nullptr), diagnostics);
      EXPECT_NE(rendezvous.Install(target.code()), nullptr);
      outcome.result = RuntimeRendezvous::GameMainWrapperDetour(0x1234);
      diagnostics.FlushAttachJournal();
      outcome.records = ParseRecords(ReadAll(local_ + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl"));
      RuntimeRendezvous::ResetActiveForTest();
    }
    outcome.stderr_text = capture.Text();
    const HMODULE mapped = GetModuleHandleW(runtime_path_.c_str());
    outcome.runtime_mapped = mapped != nullptr;
    // The stand-in is unloaded again so the next test maps its own file.
    for (int i = 0; mapped != nullptr && i < 8 && GetModuleHandleW(runtime_path_.c_str()) != nullptr; ++i) {
      FreeLibrary(mapped);
    }
    return outcome;
  }

  std::string fixtures_;
  nlohmann::json manifest_;
  nevr_plugin_signature::Policy policy_;
  std::wstring root_, install_, local_, nevr_root_, runtime_path_;
  HMODULE host_ = nullptr;
  std::string host_sha256_;
};

TEST_F(HostRuntimeVerify, ASignedRuntimeIsAcceptedLoadedAndNamedInTheVersionsRecord) {
  StageCopy("nevr-runtime.signed");
  const std::string runtime_sha256 = Sha256Hex(ReadBytes(runtime_path_));
  ASSERT_EQ(runtime_sha256.size(), 64u);
  const Outcome outcome = Run();

  EXPECT_EQ(outcome.result, kOriginalResult) << "the runtime's game_main ran and returned the original's result";
  EXPECT_TRUE(g_events.empty()) << "no stop and no dialog for an accepted runtime";
  const nlohmann::json load = Only(outcome, "runtime_load");
  EXPECT_EQ(load.at("outcome"), "loaded");
  EXPECT_EQ(load.at("verify"), "accepted");
  EXPECT_FALSE(load.contains("verify_reason"));

  const nlohmann::json versions = Only(outcome, "versions");
  EXPECT_EQ(versions.at("runtime_sha256"), runtime_sha256);
  EXPECT_EQ(versions.at("runtime_version"), "7.7.7") << "the version is the one the file's NEVR-BUILD literal carries";
  EXPECT_EQ(versions.at("host_sha256"), host_sha256_);
  EXPECT_NE(versions.at("host_version"), "unavailable");
  EXPECT_EQ(versions.at("host_build"), nevr_host::BuildWord()) << "the record says which kind of host ran (#1122)";
  EXPECT_NE(versions.at("runtime_path"), "");
  EXPECT_LT(IndexOf(outcome.records, "versions"), IndexOf(outcome.records, "runtime_load"))
      << "the versions record is written after the verdict and before the load's own record";
  EXPECT_EQ(IndexOf(outcome.records, "game_stop"), -1);
  EXPECT_EQ(outcome.stderr_text, "") << "a run that loaded the runtime writes no NEVR.STOP line";
}

// The old layout (ADR 0012, "The one-time move" and "Updating only the host"): a runtime left beside the host in the
// game's folder is not found. The host looks in the nevr root only, so a host updated alone over an old install stops
// with the "run the installer" words and names the root as the path it searched; it never maps the stale file.
TEST_F(HostRuntimeVerify, ARuntimeLeftBesideTheHostIsNotFoundAndTheGameStopsNamingTheRoot) {
  const std::wstring stale = install_ + L"\\nevr-runtime.dll";
  ASSERT_TRUE(CopyFileW(FixtureFile("nevr-runtime.signed").c_str(), stale.c_str(), FALSE));
  const Outcome outcome = Run();
  const bool stale_mapped = GetModuleHandleW(stale.c_str()) != nullptr;
  DeleteFileW(stale.c_str());

  EXPECT_EQ(g_events, (std::vector<std::string>{"notice", "stop"}));
  EXPECT_EQ(g_stop_code, 78u);
  ASSERT_EQ(g_notices.size(), 1u);
  EXPECT_NE(g_notices[0].find(L"nevr-runtime.dll is missing"), std::wstring::npos);
  EXPECT_FALSE(outcome.runtime_mapped);
  EXPECT_FALSE(stale_mapped) << "the signed file beside the host was never mapped";
  EXPECT_EQ(g_before_load_calls.load(), 0);
  const nlohmann::json stop = Only(outcome, "game_stop");
  EXPECT_EQ(stop.at("reason"), "runtime_absent");
  const std::string searched = stop.at("runtime_path").get<std::string>();
  EXPECT_NE(searched.find("nevr-root\\nevr-runtime.dll"), std::string::npos) << searched;
  EXPECT_EQ(searched.find("\\install\\"), std::string::npos) << "the game's folder is not where it looked: " << searched;
  EXPECT_EQ(Only(outcome, "versions").at("runtime_sha256"), "unavailable");

  // The console line names the old file and where to move it (#1252).
  const nlohmann::json line = OneStopLine(outcome);
  EXPECT_EQ(line.at("reason"), "runtime_absent");
  const std::string detail = line.at("detail");
  EXPECT_NE(detail.find("a nevr-runtime.dll is beside BugSplat64.dll in " + Utf8(install_) + ", which the host does not load"),
            std::string::npos) << detail;
  EXPECT_NE(detail.find("move it to " + Utf8(nevr_root_)), std::string::npos) << detail;
  EXPECT_EQ(stop.at("detail"), line.at("detail")) << "the log and the console say the same";
}

TEST_F(HostRuntimeVerify, AMissingRuntimeStopsTheGameWithCode78AndSaysWhichPathWasSearched) {
  const Outcome outcome = Run();

  EXPECT_EQ(g_events, (std::vector<std::string>{"notice", "stop"})) << "told once, then the process ends";
  EXPECT_EQ(g_stop_code, 78u);
  ASSERT_EQ(g_notices.size(), 1u);
  EXPECT_NE(g_notices[0].find(L"nevr-runtime.dll is missing"), std::wstring::npos);
  EXPECT_FALSE(outcome.runtime_mapped);
  EXPECT_EQ(g_before_load_calls.load(), 0);

  const nlohmann::json versions = Only(outcome, "versions");
  EXPECT_EQ(versions.at("runtime_sha256"), "unavailable");
  EXPECT_EQ(versions.at("runtime_version"), "unavailable");
  EXPECT_EQ(versions.at("host_sha256"), host_sha256_);
  EXPECT_TRUE(versions.contains("runtime_path"));
  const nlohmann::json load = Only(outcome, "runtime_load");
  EXPECT_EQ(load.at("outcome"), "runtime_absent");
  EXPECT_FALSE(load.contains("verify")) << "nothing was checked: there was no file";
  const nlohmann::json stop = Only(outcome, "game_stop");
  EXPECT_EQ(stop.at("reason"), "runtime_absent");
  EXPECT_EQ(stop.at("exit_code"), 78);
  EXPECT_EQ(stop.at("notice"), "shown");
  EXPECT_EQ(stop.at("runtime_path"), versions.at("runtime_path"));

  // The console line (#1252): once, with the exit code, both versions, the path searched and the log.
  const nlohmann::json line = OneStopLine(outcome);
  EXPECT_EQ(line.at("tag"), "NEVR.STOP");
  EXPECT_EQ(line.at("stopped"), true);
  EXPECT_EQ(line.at("code"), 78);
  EXPECT_EQ(line.at("reason"), "runtime_absent");
  EXPECT_EQ(line.at("host_version"), nevr_host::Version());
  EXPECT_EQ(line.at("runtime_version"), "not loaded");
  EXPECT_EQ(line.at("detail"), "nevr-runtime.dll is missing; path=" + versions.at("runtime_path").get<std::string>());
  EXPECT_EQ(std::string(line.at("detail")).find("beside"), std::string::npos) << "nothing is beside the host";
  EXPECT_EQ(line.at("log"), Utf8(g_log_path));
  // The record carries the same facts, and the dialog the version and the log.
  EXPECT_EQ(stop.at("stopped"), true);
  EXPECT_EQ(stop.at("host_version"), line.at("host_version"));
  EXPECT_EQ(stop.at("runtime_version"), line.at("runtime_version"));
  EXPECT_EQ(stop.at("detail"), line.at("detail"));
  EXPECT_EQ(stop.at("log"), line.at("log"));
  EXPECT_NE(g_notices[0].find(L"\nLog: " + g_log_path), std::wstring::npos);
  EXPECT_NE(g_notices[0].find(Widen(std::string("nEVR ") + nevr_host::Version())), std::wstring::npos);
}

// What a refusal does, whatever the cause: the file is never mapped, the one dialog, code 78, and the records in order.
void ExpectRefused(const Outcome& outcome, const std::string& host_sha256, const char* reason_prefix,
                   const char* notice_words) {
  EXPECT_FALSE(outcome.runtime_mapped) << "a refused runtime is never mapped";
  EXPECT_EQ(g_before_load_calls.load(), 0) << "the load was never attempted";
  EXPECT_EQ(g_events, (std::vector<std::string>{"notice", "stop"}));
  EXPECT_EQ(g_stop_code, 78u);
  ASSERT_EQ(g_notices.size(), 1u);
  EXPECT_NE(g_notices[0].find(Widen(notice_words)), std::wstring::npos) << "the notice says: " << notice_words;

  const nlohmann::json versions = Only(outcome, "versions");
  EXPECT_EQ(versions.at("runtime_sha256"), "unavailable") << "the facts of a refused file are not read out as if it ran";
  EXPECT_EQ(versions.at("runtime_version"), "unavailable");
  EXPECT_EQ(versions.at("host_sha256"), host_sha256);
  EXPECT_TRUE(versions.contains("runtime_path"));
  const nlohmann::json load = Only(outcome, "runtime_load");
  EXPECT_EQ(load.at("outcome"), "runtime_refused");
  EXPECT_EQ(load.at("verify"), "refused");
  EXPECT_EQ(load.at("degraded_action"), "stop_game");
  EXPECT_EQ(load.value("verify_reason", "").rfind(reason_prefix, 0), 0u) << load.value("verify_reason", "");
  const nlohmann::json stop = Only(outcome, "game_stop");
  EXPECT_EQ(stop.at("reason"), "runtime_refused");
  EXPECT_EQ(stop.at("exit_code"), 78);
  EXPECT_LT(IndexOf(outcome.records, "versions"), IndexOf(outcome.records, "runtime_load"));
  EXPECT_LT(IndexOf(outcome.records, "runtime_load"), IndexOf(outcome.records, "game_stop"));

  // The console line (#1252): the verifier's own words first, and no version for a file that was never read.
  const nlohmann::json line = OneStopLine(outcome);
  EXPECT_EQ(line.at("stopped"), true);
  EXPECT_EQ(line.at("code"), 78);
  EXPECT_EQ(line.at("reason"), "runtime_refused");
  EXPECT_EQ(line.at("runtime_version"), "not loaded");
  EXPECT_EQ(line.at("host_version"), nevr_host::Version());
  EXPECT_EQ(line.value("detail", "").rfind(reason_prefix, 0), 0u) << line.value("detail", "");
  EXPECT_EQ(stop.at("detail"), line.at("detail"));
}

TEST_F(HostRuntimeVerify, AnUnsignedRuntimeIsRefusedNeverMappedAndStopsTheGame) {
  StageCopy("nevr-runtime.unsigned");
  ExpectRefused(Run(), host_sha256_, "unsigned", "not signed");
}

TEST_F(HostRuntimeVerify, ATamperedRuntimeIsRefusedNeverMappedAndStopsTheGame) {
  StageTampered();
  ExpectRefused(Run(), host_sha256_, "digest mismatch", "changed after it was signed");
}

TEST_F(HostRuntimeVerify, ARuntimeSignedWithAnotherSignersProfileIsRefused) {
  StageCopy("nevr-runtime.wrong_eku");
  ExpectRefused(Run(), host_sha256_, "EKU", "signature");
}

TEST_F(HostRuntimeVerify, ARuntimeSignedByARootThatIsNotPinnedIsRefused) {
  policy_.rootDer.clear();  // the fixture root is no longer the pinned one: the chain does not end at a pinned root
  const std::vector<unsigned char> other = ReadBytes(WinPath(fixtures_ + "/other_root.der"));
  ASSERT_FALSE(other.empty());
  policy_.rootDer.assign(other.begin(), other.end());
  StageCopy("nevr-runtime.signed");
  // The verifier reports the first failing step of the chain it builds, here the time stamp's. A time stamp it cannot
  // place is told as the PC's clock (StopNoticeTextFor, #1533), whatever the chain status behind it.
  ExpectRefused(Run(), host_sha256_, "timestamp invalid", "date or time is wrong");
}

TEST_F(HostRuntimeVerify, ARuntimeThatCannotBeOpenedForReadingIsRefusedAsUnreadable) {
  StageCopy("nevr-runtime.signed");
  // Held open with no sharing at all: the host's own open for reading fails with a sharing violation.
  const HANDLE blocker = CreateFileW(runtime_path_.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(blocker, INVALID_HANDLE_VALUE);
  const Outcome outcome = Run();
  CloseHandle(blocker);
  ExpectRefused(outcome, host_sha256_, "unreadable", "could not be read");
}

TEST_F(HostRuntimeVerify, ADevBuildLoadsAnUnsignedRuntimeAndSaysSoInTheRecord) {
  nevr_host::SetDevBuildForTest(1);
  StageCopy("nevr-runtime.unsigned");
  const Outcome outcome = Run();

  EXPECT_EQ(outcome.result, kOriginalResult);
  EXPECT_TRUE(g_events.empty());
  const nlohmann::json load = Only(outcome, "runtime_load");
  EXPECT_EQ(load.at("outcome"), "loaded");
  EXPECT_EQ(load.at("verify"), "unsigned_allowed");
  EXPECT_EQ(load.value("verify_reason", "").rfind("unsigned", 0), 0u);
  const nlohmann::json versions = Only(outcome, "versions");
  EXPECT_EQ(versions.at("runtime_sha256"), Sha256Hex(ReadBytes(runtime_path_))) << "a loaded runtime is named";
}

// A runtime the host read but could not start carries its version into the stop facts: the zero host context fails the
// fixture's own validation, so initialize returns 1 after the file was read and mapped.
TEST_F(HostRuntimeVerify, ARuntimeThatLoadsAndFailsToInitializeStatesItsVersionInTheStopFacts) {
  StageCopy("nevr-runtime.signed");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local_.c_str(), nullptr, nullptr);
  nevr_host::RuntimeLoader loader;
  loader.Prepare();
  NevrHostContext host{};
  const nevr_host::RuntimeLoadResult result = loader.Load(host, diagnostics);
  const HMODULE mapped = GetModuleHandleW(runtime_path_.c_str());
  for (int i = 0; mapped != nullptr && i < 8 && GetModuleHandleW(runtime_path_.c_str()) != nullptr; ++i) FreeLibrary(mapped);

  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kInitFailed);
  EXPECT_EQ(result.runtime_version, "7.7.7") << "the version is the one the file's NEVR-BUILD literal carries";
  nevr_host::RuntimeStopText text;
  nevr_host::DescribeRuntimeStop(diagnostics, result, text);
  EXPECT_STREQ(text.facts.reason, "init_failed");
  EXPECT_STREQ(text.facts.runtime_version, "7.7.7");
}

TEST_F(HostRuntimeVerify, ADevBuildStillRefusesAFileItCannotRead) {
  nevr_host::SetDevBuildForTest(1);
  StageCopy("nevr-runtime.signed");
  const HANDLE blocker = CreateFileW(runtime_path_.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(blocker, INVALID_HANDLE_VALUE);
  const Outcome outcome = Run();
  CloseHandle(blocker);
  EXPECT_FALSE(outcome.runtime_mapped);
  EXPECT_EQ(g_stop_code, 78u);
  EXPECT_EQ(Only(outcome, "runtime_load").at("outcome"), "runtime_refused");
}

TEST_F(HostRuntimeVerify, TheFileIsHeldAgainstAWriteAndADeleteBetweenTheVerdictAndTheLoad) {
  StageCopy("nevr-runtime.signed");
  const Outcome outcome = Run();
  EXPECT_EQ(outcome.result, kOriginalResult);
  ASSERT_EQ(g_before_load_calls.load(), 1) << "the load ran once, after the verdict";
  EXPECT_FALSE(g_write_open_succeeded) << "a writer opened the verified file before it was mapped";
  EXPECT_EQ(g_write_open_error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
  EXPECT_FALSE(g_delete_succeeded) << "the verified file was deleted before it was mapped";
  EXPECT_TRUE(g_versions_written_before_load) << "the versions record was not on disk when the load began";
}

TEST_F(HostRuntimeVerify, AServerIsRefusedTheSameWayWithTheRecordsAndNoDialog) {
  g_command_line = "echovr.exe -server -headless";
  StageCopy("nevr-runtime.unsigned");
  const Outcome outcome = Run();
  EXPECT_FALSE(outcome.runtime_mapped);
  EXPECT_EQ(g_events, (std::vector<std::string>{"stop"})) << "no dialog on a server: nobody is there to close it";
  EXPECT_EQ(g_stop_code, 78u);
  EXPECT_EQ(Only(outcome, "game_stop").at("notice"), "suppressed_server_or_headless");
  EXPECT_EQ(Only(outcome, "runtime_load").at("outcome"), "runtime_refused");
  EXPECT_EQ(Only(outcome, "versions").at("runtime_sha256"), "unavailable");
  // A server has no dialog; the console line is what its operator reads.
  const nlohmann::json line = OneStopLine(outcome);
  EXPECT_EQ(line.at("reason"), "runtime_refused");
  EXPECT_EQ(line.at("code"), 78);
}

}  // namespace
