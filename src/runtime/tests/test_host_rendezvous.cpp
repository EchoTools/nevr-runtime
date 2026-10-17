// The validation half of the host's GameMainWrapper rendezvous
// (runtime/host/runtime_rendezvous.h): the host never writes a detour over bytes it
// did not pin. The detour itself, the dispatch order and the one-original rule are
// measured by the child-process harnesses in tests/bootstrap_attach and
// tests/rendezvous.

#include "runtime/host/runtime_rendezvous.h"

#include <gtest/gtest.h>

#include <MinHook.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using nevr_host::AttachOutcome;
using nevr_host::AttachStep;
using nevr_host::BootstrapDiagnostics;
using nevr_host::RuntimeRendezvous;

std::wstring MakeTempDir() {
  wchar_t base[MAX_PATH] = {};
  EXPECT_GT(GetTempPathW(MAX_PATH, base), 0u);
  wchar_t dir[MAX_PATH] = {};
  swprintf(dir, MAX_PATH, L"%ls\\nevr-rdv-%lu-%llu", base,
           static_cast<unsigned long>(GetCurrentProcessId()),
           static_cast<unsigned long long>(GetTickCount64()));
  EXPECT_NE(CreateDirectoryW(dir, nullptr), FALSE);
  return std::wstring(dir);
}

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

}  // namespace

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

std::vector<nlohmann::json> Records(BootstrapDiagnostics& diagnostics, const std::wstring& local) {
  diagnostics.FlushAttachJournal();
  const std::string text = ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
  std::vector<nlohmann::json> records;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    records.push_back(nlohmann::json::parse(text.substr(start, end - start)));
    start = end + 1;
  }
  return records;
}

// What is in the file now, without flushing anything first.
std::vector<nlohmann::json> RecordsOnDisk(const std::wstring& local) {
  const std::string text = ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
  std::vector<nlohmann::json> records;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    records.push_back(nlohmann::json::parse(text.substr(start, end - start)));
    start = end + 1;
  }
  return records;
}

const nlohmann::json* Step(const std::vector<nlohmann::json>& records, const char* step) {
  for (const nlohmann::json& record : records) {
    if (record.value("step", "") == step) return &record;
  }
  return nullptr;
}

}  // namespace

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
