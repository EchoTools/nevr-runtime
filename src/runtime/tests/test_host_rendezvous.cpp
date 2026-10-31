// The validation half of the host's GameMainWrapper rendezvous
// (runtime/host/runtime_rendezvous.h): the host never writes a detour over bytes it
// did not pin. The detour itself, the dispatch order and the one-original rule are
// measured by the child-process harnesses in tests/bootstrap_attach and
// tests/rendezvous.

#include "runtime/host/host_identity.h"
#include "runtime/host/runtime_rendezvous.h"
#include "runtime/host/stop_notice.h"
#include "runtime/tests/stderr_capture.h"

#include <gtest/gtest.h>

#include <MinHook.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using nevr_host::AttachOutcome;
using nevr_host::AttachStep;
using nevr_host::BootstrapDiagnostics;
using nevr_host::RuntimeRendezvous;

// The directory name is built from a tick the caller passes, so a test can hold the tick fixed. A process-wide
// sequence in the name keeps two calls made in one tick on two directories.
std::wstring MakeTempDirWithTick(ULONGLONG tick) {
  static LONG sequence = 0;
  wchar_t base[MAX_PATH] = {};
  EXPECT_GT(GetTempPathW(MAX_PATH, base), 0u);
  const LONG n = InterlockedIncrement(&sequence);
  wchar_t dir[MAX_PATH] = {};
  swprintf(dir, MAX_PATH, L"%ls\\nevr-rdv-%lu-%llu-%ld", base,
           static_cast<unsigned long>(GetCurrentProcessId()),
           static_cast<unsigned long long>(tick), n);
  const BOOL created = CreateDirectoryW(dir, nullptr);
  const DWORD error = GetLastError();
  EXPECT_NE(created, FALSE) << "CreateDirectoryW failed, GetLastError=" << error;
  return std::wstring(dir);
}

std::wstring MakeTempDir() { return MakeTempDirWithTick(GetTickCount64()); }

std::string ReadAll(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string out;
  char buffer[512] = {};
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) {
    out.append(buffer, read);
  }
  CloseHandle(file);
  return out;
}

// Readable memory holding the pinned prologue, optionally with one byte changed.
struct Target {
  unsigned char bytes[nevr_host::kGameMainWrapperPrologueSize + 8] = {};
  explicit Target(int mutated_offset = -1) {
    std::memcpy(bytes, nevr_host::kGameMainWrapperPrologue, nevr_host::kGameMainWrapperPrologueSize);
    if (mutated_offset >= 0) bytes[mutated_offset] ^= 0x01;
  }
};


// No test in this file has a runtime to load: the folder the host looks in is an empty one, so every load ends at
// runtime_absent whatever the Wine prefix's own nEVR root holds. The production rule is tested in test_host_runtime_load.
class EmptyRuntimeFolder : public ::testing::Environment {
 public:
  void SetUp() override {
    folder_ = MakeTempDir();
    nevr_host::SetRuntimeDirForTest(folder_.c_str());
  }
  void TearDown() override { nevr_host::SetRuntimeDirForTest(nullptr); }

 private:
  std::wstring folder_;
};

const ::testing::Environment* const g_empty_runtime_folder = ::testing::AddGlobalTestEnvironment(new EmptyRuntimeFolder);

}  // namespace

// Directories made inside one GetTickCount64 tick must not share a name: three calls with one fixed tick,
// the case two calls in the same tick produce.
TEST(HostRendezvous, TempDirsMadeInOneTickAreDistinctAndAllCreated) {
  const ULONGLONG tick = GetTickCount64();
  std::vector<std::wstring> dirs;
  for (int i = 0; i < 3; ++i) dirs.push_back(MakeTempDirWithTick(tick));
  EXPECT_NE(dirs[0], dirs[1]);
  EXPECT_NE(dirs[0], dirs[2]);
  EXPECT_NE(dirs[1], dirs[2]);
  for (const std::wstring& dir : dirs) RemoveDirectoryW(dir.c_str());
}

TEST(HostRendezvous, ThePinnedPrologueMatchesAndEveryChangedByteDoesNot) {
  const Target pinned;
  EXPECT_TRUE(RuntimeRendezvous::PrologueMatches(pinned.bytes));
  for (std::size_t offset = 0; offset < nevr_host::kGameMainWrapperPrologueSize; ++offset) {
    const Target changed(static_cast<int>(offset));
    EXPECT_FALSE(RuntimeRendezvous::PrologueMatches(changed.bytes)) << "offset " << offset;
  }
}

TEST(HostRendezvous, UnreadableOrNullTargetsAreRejectedRatherThanFaulting) {
  EXPECT_FALSE(RuntimeRendezvous::PrologueMatches(nullptr));
  // A reserved, uncommitted page: ReadProcessMemory fails, a direct read would fault.
  void* reserved = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
  ASSERT_NE(reserved, nullptr);
  EXPECT_FALSE(RuntimeRendezvous::PrologueMatches(reserved));
  VirtualFree(reserved, 0, MEM_RELEASE);
}

TEST(HostRendezvous, AMismatchedPrologueIsNotHookedAndIsJournalledWithTheBytesFound) {
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);

  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(nullptr), GetModuleHandleW(nullptr), diagnostics);
  const Target changed(7);
  EXPECT_EQ(rendezvous.Install(const_cast<unsigned char*>(changed.bytes)), nullptr);
  EXPECT_FALSE(rendezvous.Installed());
  EXPECT_TRUE(diagnostics.AttachDegraded());
  // The bytes were left alone.
  EXPECT_EQ(changed.bytes[7], static_cast<unsigned char>(nevr_host::kGameMainWrapperPrologue[7] ^ 0x01));

  diagnostics.FlushAttachJournal();
  const std::string line = ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
  const nlohmann::json record = nlohmann::json::parse(line.substr(0, line.find('\n')));
  EXPECT_EQ(record.at("step"), "prologue_check");
  EXPECT_EQ(record.at("outcome"), "mismatch");
  EXPECT_EQ(record.at("actual_prologue"), "4883ec3848c744252000000000");
  EXPECT_EQ(record.at("expected_prologue"), "4883ec3848c744242000000000");
  EXPECT_EQ(record.at("rva"), "0xcd510");
}

// The pinned bytes are the ones in echovr.exe at the wrapper: sub rsp,0x38 then
// mov qword [rsp+0x20],0 (13 bytes, ending before the CALL rel32 to GameMain).
TEST(HostRendezvous, TheWrapperTargetIsThePinnedRvaAndThirteenBytes) {
  EXPECT_EQ(nevr_host::kGameMainWrapperRva, 0xCD510u);
  EXPECT_EQ(nevr_host::kGameMainWrapperPrologueSize, 13u);
  const unsigned char expected[13] = {0x48, 0x83, 0xec, 0x38, 0x48, 0xc7, 0x44,
                                      0x24, 0x20, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(std::memcmp(nevr_host::kGameMainWrapperPrologue, expected, sizeof(expected)), 0);
}

TEST(HostRendezvous, AnUnreadableTargetIsJournalledWithoutActualBytes) {
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);

  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(nullptr), GetModuleHandleW(nullptr), diagnostics);
  EXPECT_EQ(rendezvous.Install(nullptr), nullptr);

  diagnostics.FlushAttachJournal();
  const std::string line = ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
  const nlohmann::json record = nlohmann::json::parse(line.substr(0, line.find('\n')));
  EXPECT_EQ(record.at("outcome"), "mismatch");
  EXPECT_FALSE(record.contains("actual_prologue"));
}

// --- Install, the install wait and the detour's guards, on a real executable target ----------

namespace {

using InstallWait = nevr_host::RuntimeRendezvous::InstallWait;

// Executable memory that starts with the pinned prologue, so MinHook can really hook it.
class ExecutableTarget {
 public:
  ExecutableTarget() {
    code_ = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (code_ == nullptr) return;
    std::memcpy(code_, nevr_host::kGameMainWrapperPrologue, nevr_host::kGameMainWrapperPrologueSize);
    code_[nevr_host::kGameMainWrapperPrologueSize] = 0xC3;
    std::memset(code_ + nevr_host::kGameMainWrapperPrologueSize + 1, 0xCC, 32);
  }
  ~ExecutableTarget() {
    MH_Uninitialize();  // before the hooked memory goes: it restores the target's bytes
    if (code_ != nullptr) VirtualFree(code_, 0, MEM_RELEASE);
  }
  unsigned char* code() const { return code_; }

 private:
  unsigned char* code_ = nullptr;
};

// Puts the test seams back and takes MinHook down again, whatever the test did.
struct RendezvousSandbox {
  ~RendezvousSandbox() {
    RuntimeRendezvous::Seams() = RuntimeRendezvous::TestSeams{};
    MH_Uninitialize();
  }
};

// What is in the file now, without flushing anything first. Only a line that ends in a newline is a
// record: the writer ends every record with one, so a last line without it is a write cut short and is
// not read.
std::vector<nlohmann::json> RecordsOnDisk(const std::wstring& local) {
  const std::string text = ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
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

std::vector<nlohmann::json> Records(BootstrapDiagnostics& diagnostics, const std::wstring& local) {
  diagnostics.FlushAttachJournal();
  return RecordsOnDisk(local);
}

const nlohmann::json* Step(const std::vector<nlohmann::json>& records, const char* step) {
  for (const nlohmann::json& record : records) {
    if (record.value("step", "") == step) return &record;
  }
  return nullptr;
}

}  // namespace

// A last line with no newline (a write cut short) ends the read; it is not looped over.
TEST(HostRendezvousRecords, ALastLineWithoutANewlineEndsTheReadInsteadOfLoopingForever) {
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  const char complete[] = "{\"event\":\"complete\"}\n";
  const char cut[] = "{\"event\":\"cut_short\"}";  // parses, has no newline
  diagnostics.AppendLine(complete, sizeof(complete) - 1);
  diagnostics.AppendLine(cut, sizeof(cut) - 1);
  const std::vector<nlohmann::json> records = RecordsOnDisk(local);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("event"), "complete");
}

TEST(HostRendezvousInstall, ReadsTheHookBytesAfterEnableAndMarksThemValid) {
  RendezvousSandbox sandbox;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ExecutableTarget target;
  ASSERT_NE(target.code(), nullptr);
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(nullptr), GetModuleHandleW(nullptr), diagnostics);
  ASSERT_NE(rendezvous.Install(target.code()), nullptr);

  const NevrHostContext& context = rendezvous.Context();
  EXPECT_EQ(context.wrapper_hook_bytes_valid, 1u);
  EXPECT_EQ(context.wrapper_hook_target, target.code());
  EXPECT_EQ(context.wrapper_hook_bytes[0], 0xE9);  // the host's own jump, not the pinned prologue
  EXPECT_EQ(std::memcmp(context.wrapper_hook_bytes, target.code(), nevr_runtime_abi::kHookBytesSize), 0);
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&context), nevr_runtime_abi::Status::kOk);
  EXPECT_TRUE(nevr_runtime_abi::HookRecordUsable(&context));
  EXPECT_NE(context.host_module, nullptr);
  const auto records = Records(diagnostics, local);
  ASSERT_NE(Step(records, "hook_install"), nullptr);
  EXPECT_FALSE(Step(records, "hook_install")->contains("hook_bytes"));
}

TEST(HostRendezvousInstall, AFailedBytesReadIsOnTheInstallRecordAndTheContextSaysUnread) {
  RendezvousSandbox sandbox;
  RuntimeRendezvous::Seams().fail_hook_bytes_read = true;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ExecutableTarget target;
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(nullptr), GetModuleHandleW(nullptr), diagnostics);
  ASSERT_NE(rendezvous.Install(target.code()), nullptr);  // the hook is live; only the record is missing

  const NevrHostContext& context = rendezvous.Context();
  EXPECT_EQ(context.wrapper_hook_bytes_valid, 0u);
  for (unsigned char byte : context.wrapper_hook_bytes) EXPECT_EQ(byte, 0);
  EXPECT_EQ(nevr_runtime_abi::ValidateHostContext(&context), nevr_runtime_abi::Status::kOk);
  EXPECT_FALSE(nevr_runtime_abi::HookRecordUsable(&context));
  EXPECT_TRUE(diagnostics.AttachDegraded());
  const auto records = Records(diagnostics, local);
  const nlohmann::json* install = Step(records, "hook_install");
  ASSERT_NE(install, nullptr);
  EXPECT_EQ(install->at("outcome"), "enabled");
  EXPECT_EQ(install->at("hook_bytes"), "unread");
}

TEST(HostRendezvousInstall, TheInstallWaitIsSignalledOnceTheInstallHasExitedAndIsRecorded) {
  RendezvousSandbox sandbox;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ExecutableTarget target;
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(nullptr), GetModuleHandleW(nullptr), diagnostics);
  ASSERT_NE(rendezvous.Install(target.code()), nullptr);
  EXPECT_EQ(rendezvous.AwaitInstall(), InstallWait::kSignalled);
  const auto records = Records(diagnostics, local);
  ASSERT_NE(Step(records, "install_wait"), nullptr);
  EXPECT_EQ(Step(records, "install_wait")->at("outcome"), "signalled");
}

TEST(HostRendezvousInstall, AnEventThatCannotBeCreatedIsRecordedAsUnavailableWithWhatTheLoadDoesNext) {
  RendezvousSandbox sandbox;
  RuntimeRendezvous::Seams().fail_event_create = true;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ExecutableTarget target;
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(nullptr), GetModuleHandleW(nullptr), diagnostics);
  ASSERT_NE(rendezvous.Install(target.code()), nullptr);  // the install does not depend on the event
  EXPECT_EQ(rendezvous.AwaitInstall(), InstallWait::kUnavailable);
  EXPECT_TRUE(diagnostics.AttachDegraded());
  const auto records = Records(diagnostics, local);
  const nlohmann::json* wait = Step(records, "install_wait");
  ASSERT_NE(wait, nullptr);
  EXPECT_EQ(wait->at("outcome"), "unavailable");
  EXPECT_EQ(wait->at("next"), "load_proceeds_unsynchronized_and_the_journal_is_flushed_again_after_load");
}

TEST(HostRendezvousInstall, AnInstallThatNeverFinishesTimesOutAndIsRecordedWithWhatTheLoadDoesNext) {
  RendezvousSandbox sandbox;
  RuntimeRendezvous::Seams().install_wait_ms = 30;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(nullptr), GetModuleHandleW(nullptr), diagnostics);
  HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // nobody signals it
  rendezvous.SetInstallEventForTest(never);
  EXPECT_EQ(rendezvous.AwaitInstall(), InstallWait::kTimeout);
  CloseHandle(never);
  const auto records = Records(diagnostics, local);
  const nlohmann::json* wait = Step(records, "install_wait");
  ASSERT_NE(wait, nullptr);
  EXPECT_EQ(wait->at("outcome"), "timeout");
  EXPECT_EQ(wait->at("next"), "load_proceeds_unsynchronized_and_the_journal_is_flushed_again_after_load");
}

// Unreachable in production (Install publishes the original before the jump is live), so the guard
// is reached here by calling the detour on a rendezvous that never installed.
TEST(HostRendezvousInstall, ADetourEnteredWithoutAHookReturnsZeroAndLeavesARecord) {
  RendezvousSandbox sandbox;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(nullptr), GetModuleHandleW(nullptr), diagnostics);
  EXPECT_EQ(RuntimeRendezvous::GameMainWrapperDetour(0x1234), 0);
  // On disk without anything else flushing: nothing follows this return, so the detour flushes it.
  const auto records = RecordsOnDisk(local);
  const nlohmann::json* defect = Step(records, "detour_entry");
  ASSERT_NE(defect, nullptr);
  EXPECT_EQ(defect->at("outcome"), "defect");
  EXPECT_EQ(defect->at("next"), "returned_0_without_calling_the_original");
}

TEST(HostRendezvousInstall, ADetourEnteredWithNoRendezvousAtAllReturnsZero) {
  RendezvousSandbox sandbox;
  RuntimeRendezvous::ResetActiveForTest();
  EXPECT_EQ(RuntimeRendezvous::GameMainWrapperDetour(0x1234), 0);
}

// --- the stop: a runtime that did not load ends the game (docs/adr/0012-static-install-root.md) ----

namespace {

std::atomic<int> g_stops{0};
std::atomic<std::uint32_t> g_stop_code{0};
void CountStop(std::uint32_t exit_code) {
  g_stop_code.store(exit_code);
  g_stops.fetch_add(1);
}

// Executable memory that starts with the pinned prologue and then returns 0x12345678 as a function that
// leaves its frame, so the trampoline MinHook builds can really be called.
class ReturningTarget {
 public:
  ReturningTarget() {
    code_ = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (code_ == nullptr) return;
    std::memcpy(code_, nevr_host::kGameMainWrapperPrologue, nevr_host::kGameMainWrapperPrologueSize);
    const unsigned char tail[] = {0xB8, 0x78, 0x56, 0x34, 0x12,  // mov eax, 0x12345678
                                  0x48, 0x83, 0xC4, 0x38,        // add rsp, 0x38
                                  0xC3};                         // ret
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

}  // namespace

TEST(HostRendezvousStop, ARuntimeThatDidNotLoadStopsTheGameWithCode78AndOneRecordHoweverOftenTheDetourRuns) {
  RendezvousSandbox sandbox;
  g_stops.store(0);
  g_stop_code.store(0);
  RuntimeRendezvous::Seams().stop_game = &CountStop;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ReturningTarget target;
  ASSERT_NE(target.code(), nullptr);
  RuntimeRendezvous rendezvous;
  // The runtime folder is empty (EmptyRuntimeFolder): the load ends at runtime_absent.
  rendezvous.Prepare(GetModuleHandleW(L"kernel32.dll"), GetModuleHandleW(nullptr), diagnostics);
  ASSERT_NE(rendezvous.Install(target.code()), nullptr);

  // The seam lets the original run after the stop, so a harness can look on; production ends the process.
  EXPECT_EQ(RuntimeRendezvous::GameMainWrapperDetour(0x1234), 0x12345678);
  EXPECT_EQ(RuntimeRendezvous::GameMainWrapperDetour(0x1234), 0x12345678);
  EXPECT_EQ(g_stops.load(), 2);
  EXPECT_EQ(g_stop_code.load(), 78u);
  EXPECT_EQ(rendezvous.RuntimeApi(), nullptr);

  int stop_records = 0;
  for (const nlohmann::json& record : Records(diagnostics, local)) {
    if (record.value("event", "") != "game_stop") continue;
    ++stop_records;
    EXPECT_EQ(record.at("reason"), "runtime_absent");
    EXPECT_EQ(record.at("exit_code"), 78);
  }
  EXPECT_EQ(stop_records, 1);
  RuntimeRendezvous::ResetActiveForTest();
}

// The player is told once, plainly, when the runtime did not load (decision 3 of ADR 0012 step 2; #854 owns the
// final wording and the verification). Nobody is at the screen of a server or any headless run, and a modal box
// there would hold the process, so those get the record only.
namespace {

std::vector<std::string> g_events;
std::vector<std::wstring> g_notice_texts;
const char* g_command_line = "";
std::string g_stderr;     // what the last RunStopWith wrote to the standard error handle
std::wstring g_log_path;  // the host log of the last RunStopWith

void RecordNotice(const wchar_t* text) {
  g_events.push_back("notice");
  g_notice_texts.emplace_back(text);
}
std::uint32_t g_stop_exit_code = 0;
void RecordStop(std::uint32_t exit_code) {
  g_stop_exit_code = exit_code;
  g_events.push_back("stop");
}
const char* CommandLine() { return g_command_line; }

// Runs the detour three times against a runtime that is absent, with the given command line, and returns the
// notice decision the game_stop record carries.
std::string RunStopWith(const char* command_line, std::vector<nlohmann::json>* records_out = nullptr) {
  RendezvousSandbox sandbox;
  nevr_test::StderrCapture capture;
  g_events.clear();
  g_notice_texts.clear();
  g_command_line = command_line;
  RuntimeRendezvous::Seams().stop_game = &RecordStop;
  RuntimeRendezvous::Seams().show_stop_notice = &RecordNotice;
  RuntimeRendezvous::Seams().command_line = &CommandLine;
  RuntimeRendezvous::Seams().exit_notice_wait_ms = 0;  // the hold for a console reader is not for a unit test
  const std::wstring local = MakeTempDir();
  g_log_path = local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl";
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ReturningTarget target;
  EXPECT_NE(target.code(), nullptr);
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(L"kernel32.dll"), GetModuleHandleW(nullptr), diagnostics);
  EXPECT_NE(rendezvous.Install(target.code()), nullptr);
  for (int i = 0; i < 3; ++i) RuntimeRendezvous::GameMainWrapperDetour(0x1234);
  g_stderr = capture.Text();
  std::string notice;
  const std::vector<nlohmann::json> records = Records(diagnostics, local);
  for (const nlohmann::json& record : records) {
    if (record.value("event", "") == "game_stop") notice = record.value("notice", "(none)");
  }
  if (records_out != nullptr) *records_out = records;
  RuntimeRendezvous::ResetActiveForTest();
  return notice;
}

}  // namespace

TEST(HostStopNotice, AClientWithGraphicsIsToldOnceBeforeTheProcessEndsHoweverOftenTheDetourRuns) {
  EXPECT_EQ(RunStopWith("\"C:\\Games\\echovr.exe\" -windowed"), "shown");
  EXPECT_EQ(g_notice_texts.size(), 1u);
  ASSERT_FALSE(g_notice_texts.empty());
  // The runtime folder is empty (EmptyRuntimeFolder): the notice says it is missing (test_host_runtime_verify holds the
  // words for each other cause).
  // The sentence, then which build and which log (#1252).
  const std::wstring version(nevr_host::Version(), nevr_host::Version() + std::strlen(nevr_host::Version()));
  EXPECT_EQ(g_notice_texts[0], std::wstring(L"nEVR couldn't start: nevr-runtime.dll is missing. Please run the installer.") +
                                   L"\n\nnEVR " + version + L"\nLog: " + g_log_path);
  ASSERT_GE(g_events.size(), 2u);
  EXPECT_EQ(g_events[0], "notice") << "the player is told before the process is ended";
  EXPECT_EQ(g_events[1], "stop");
}

// #1252: whatever the command line says about a dialog, the console gets the one line, once per run.
TEST(HostStopNotice, EveryStopWritesOneStopLineToStandardErrorHoweverOftenTheDetourRuns) {
  for (const char* line : {"\"C:\\Games\\echovr.exe\" -windowed", "echovr.exe -server -noovr", "echovr.exe -headless"}) {
    SCOPED_TRACE(line);
    RunStopWith(line);
    ASSERT_FALSE(g_stderr.empty());
    // The JSON line (#1252), then the one plain notice line (#1533): two lines, three detour runs.
    const std::size_t json_end = g_stderr.find('\n');
    ASSERT_NE(json_end, std::string::npos) << g_stderr;
    const std::string notice = g_stderr.substr(json_end + 1);
    EXPECT_EQ(notice.rfind("nEVR couldn't start: ", 0), 0u) << g_stderr;
    EXPECT_EQ(notice.find('\n'), notice.size() - 1)
        << "one stop line and one notice, three detour runs: " << g_stderr;
    const nlohmann::json record = nlohmann::json::parse(g_stderr.substr(0, json_end));
    EXPECT_EQ(record.at("tag"), "NEVR.STOP");
    EXPECT_EQ(record.at("event"), "stop");
    EXPECT_EQ(record.at("stopped"), true);
    EXPECT_EQ(record.at("code"), 78);
    EXPECT_EQ(record.at("reason"), "runtime_absent");
    EXPECT_EQ(record.at("host_version"), nevr_host::Version());
    EXPECT_EQ(record.at("runtime_version"), "not loaded");
    EXPECT_NE(std::string(record.at("detail")).find("nevr-runtime.dll is missing; path="), std::string::npos);
    // UTF-8, not one narrowed code unit per character: a temp path with a non-ASCII user name must still match.
    std::string expected_log(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, g_log_path.c_str(), -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, g_log_path.c_str(), -1, expected_log.data(), static_cast<int>(expected_log.size()), nullptr, nullptr);
    expected_log.resize(expected_log.size() - 1);
    EXPECT_EQ(record.at("log"), expected_log);
  }
}

TEST(HostStopNotice, AServerIsNeverShownADialogThatWouldHoldTheProcess) {
  for (const char* line : {"echovr.exe -server -noovr", "echovr.exe -SERVER", "echovr.exe --server", "echovr.exe -windowed -server"}) {
    SCOPED_TRACE(line);
    EXPECT_EQ(RunStopWith(line), "suppressed_server_or_headless");
    EXPECT_TRUE(g_notice_texts.empty());
    EXPECT_FALSE(g_events.empty());
    EXPECT_EQ(g_events[0], "stop");
  }
}

TEST(HostStopNotice, AnyHeadlessRunIsSpokenToByTheLogOnly) {
  for (const char* line : {"echovr.exe -headless", "echovr.exe -windowed -HEADLESS", "echovr.exe +headless"}) {
    SCOPED_TRACE(line);
    EXPECT_EQ(RunStopWith(line), "suppressed_server_or_headless");
    EXPECT_TRUE(g_notice_texts.empty());
  }
}

namespace {
const char* UnreadableCommandLine() { throw std::runtime_error("the command line cannot be read"); }
}  // namespace

TEST(HostStopNotice, ACommandLineThatCannotBeReadMeansNoDialogBecauseItMightHoldAServer) {
  RendezvousSandbox sandbox;
  g_events.clear();
  g_notice_texts.clear();
  RuntimeRendezvous::Seams().stop_game = &RecordStop;
  RuntimeRendezvous::Seams().show_stop_notice = &RecordNotice;
  RuntimeRendezvous::Seams().command_line = &UnreadableCommandLine;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ReturningTarget target;
  ASSERT_NE(target.code(), nullptr);
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(L"kernel32.dll"), GetModuleHandleW(nullptr), diagnostics);
  ASSERT_NE(rendezvous.Install(target.code()), nullptr);
  RuntimeRendezvous::GameMainWrapperDetour(0x1234);
  EXPECT_TRUE(g_notice_texts.empty());
  std::string notice = "(none)";
  for (const nlohmann::json& record : Records(diagnostics, local)) {
    if (record.value("event", "") == "game_stop") notice = record.value("notice", "(none)");
  }
  EXPECT_EQ(notice, "suppressed_server_or_headless");
  RuntimeRendezvous::ResetActiveForTest();
}

TEST(HostStopNotice, ANullCommandLineFromTheSystemMeansNoDialogAndTheRecordSaysSo) {
  EXPECT_EQ(RunStopWith(nullptr), "suppressed_server_or_headless");
  EXPECT_TRUE(g_notice_texts.empty());
}

// #1533: a -server whose runtime is absent is told the cause on its console, once, and still exits with 78.
TEST(HostStopNotice, AServerIsToldThePlainCauseOnItsConsoleOnceAndStillExitsWith78) {
  EXPECT_EQ(RunStopWith("echovr.exe -server"), "suppressed_server_or_headless");
  EXPECT_EQ(g_stop_exit_code, 78u);
  EXPECT_NE(g_stderr.find("nEVR couldn't start: nevr-runtime.dll is missing. Please run the installer. Detail: "),
            std::string::npos)
      << g_stderr;
  std::size_t notices = 0;
  for (std::size_t at = g_stderr.find("Detail: "); at != std::string::npos; at = g_stderr.find("Detail: ", at + 1)) {
    ++notices;
  }
  EXPECT_EQ(notices, 1u) << "one stop, one notice, however often the detour runs: " << g_stderr;
  EXPECT_TRUE(g_notice_texts.empty()) << "a server never gets the dialog";
}

TEST(HostStopNotice, ThePolicyReadsTokensNotSubstrings) {
  using nevr_host::DecideStopNotice;
  using nevr_host::StopNotice;
  EXPECT_EQ(DecideStopNotice("echovr.exe -windowed", false), StopNotice::kShow);
  EXPECT_EQ(DecideStopNotice("echovr.exe -servers", false), StopNotice::kShow);
  EXPECT_EQ(DecideStopNotice("echovr.exe -serverx -headlessly", false), StopNotice::kShow);
  EXPECT_EQ(DecideStopNotice("echovr.exe -config server", false), StopNotice::kShow) << "a value, not an option";
  EXPECT_EQ(DecideStopNotice("\"C:\\a -server\\echovr.exe\" -windowed", false), StopNotice::kShow)
      << "a path that contains the text is not the option";
  EXPECT_EQ(DecideStopNotice("echovr.exe -server", false), StopNotice::kSuppressedServerOrHeadless);
  EXPECT_EQ(DecideStopNotice("echovr.exe -noovr -headless", false), StopNotice::kSuppressedServerOrHeadless);
  EXPECT_EQ(DecideStopNotice("\"-server\"", false), StopNotice::kSuppressedServerOrHeadless)
      << "boot.cpp reads a quoted -server as a server, so the dialog stays away";
  EXPECT_EQ(DecideStopNotice("", false), StopNotice::kShow);
  EXPECT_EQ(DecideStopNotice(nullptr, false), StopNotice::kSuppressedServerOrHeadless)
      << "a command line that cannot be read might belong to a server: no dialog";
}

TEST(HostStopNotice, NothingIsShownUnderTheLoaderLock) {
  using nevr_host::DecideStopNotice;
  using nevr_host::StopNotice;
  EXPECT_EQ(DecideStopNotice("echovr.exe -windowed", true), StopNotice::kSuppressedLoaderLock);
  EXPECT_STREQ(nevr_host::StopNoticeName(StopNotice::kShow), "shown");
  EXPECT_STREQ(nevr_host::StopNoticeName(StopNotice::kSuppressedLoaderLock), "suppressed_loader_lock");
}

// The process must not end under the dialog: a second thread that reaches the stop while the first is telling the
// player waits behind it (StopGame holds one lock through the record, the notice and the stop).
namespace {

CRITICAL_SECTION g_events_lock;
std::vector<std::string> g_ordered_events;
HANDLE g_second_thread = nullptr;

void Note(const char* what) {
  EnterCriticalSection(&g_events_lock);
  g_ordered_events.emplace_back(what);
  LeaveCriticalSection(&g_events_lock);
}

DWORD WINAPI SecondArrival(LPVOID) {
  RuntimeRendezvous::GameMainWrapperDetour(0x1234);
  return 0;
}

void NoticeWithASecondThreadArriving(const wchar_t*) {
  Note("notice_open");
  g_second_thread = CreateThread(nullptr, 0, &SecondArrival, nullptr, 0, nullptr);
  Sleep(300);  // long enough for the second thread to be inside the detour and waiting on the lock
  Note("notice_closed");
}
void NoteStop(std::uint32_t) { Note("stop"); }

}  // namespace

TEST(HostStopNotice, ASecondThreadWaitsBehindTheDialogInsteadOfEndingTheProcessUnderIt) {
  RendezvousSandbox sandbox;
  InitializeCriticalSection(&g_events_lock);
  g_ordered_events.clear();
  g_second_thread = nullptr;
  g_command_line = "echovr.exe -windowed";
  RuntimeRendezvous::Seams().stop_game = &NoteStop;
  RuntimeRendezvous::Seams().show_stop_notice = &NoticeWithASecondThreadArriving;
  RuntimeRendezvous::Seams().command_line = &CommandLine;
  const std::wstring local = MakeTempDir();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ReturningTarget target;
  ASSERT_NE(target.code(), nullptr);
  RuntimeRendezvous rendezvous;
  rendezvous.Prepare(GetModuleHandleW(L"kernel32.dll"), GetModuleHandleW(nullptr), diagnostics);
  ASSERT_NE(rendezvous.Install(target.code()), nullptr);
  RuntimeRendezvous::GameMainWrapperDetour(0x1234);
  ASSERT_NE(g_second_thread, nullptr);
  ASSERT_EQ(WaitForSingleObject(g_second_thread, 10000), WAIT_OBJECT_0);
  CloseHandle(g_second_thread);
  EnterCriticalSection(&g_events_lock);
  const std::vector<std::string> events = g_ordered_events;
  LeaveCriticalSection(&g_events_lock);
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[0], "notice_open");
  EXPECT_EQ(events[1], "notice_closed") << "the second thread ended the process while the dialog was open";
  EXPECT_EQ(events[2], "stop");
  EXPECT_EQ(events[3], "stop");
  RuntimeRendezvous::ResetActiveForTest();
  DeleteCriticalSection(&g_events_lock);
}
