// The validation half of the host's PreprocessCommandLine rendezvous
// (runtime/host/runtime_rendezvous.h): the host never writes a detour over bytes it
// did not pin. The detour itself, the dispatch order and the one-original rule are
// measured by the child-process harnesses in tests/bootstrap_attach and
// tests/rendezvous.

#include "runtime/host/runtime_rendezvous.h"

#include <gtest/gtest.h>

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
  unsigned char bytes[nevr_host::kPreprocessPrologueSize + 8] = {};
  explicit Target(int mutated_offset = -1) {
    std::memcpy(bytes, nevr_host::kPreprocessPrologue, nevr_host::kPreprocessPrologueSize);
    if (mutated_offset >= 0) bytes[mutated_offset] ^= 0x01;
  }
};

}  // namespace

TEST(HostRendezvous, ThePinnedPrologueMatchesAndEveryChangedByteDoesNot) {
  const Target pinned;
  EXPECT_TRUE(RuntimeRendezvous::PrologueMatches(pinned.bytes));
  for (std::size_t offset = 0; offset < nevr_host::kPreprocessPrologueSize; ++offset) {
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
  EXPECT_EQ(changed.bytes[7], static_cast<unsigned char>(nevr_host::kPreprocessPrologue[7] ^ 0x01));

  diagnostics.FlushAttachJournal();
  const std::string line = ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
  const nlohmann::json record = nlohmann::json::parse(line.substr(0, line.find('\n')));
  EXPECT_EQ(record.at("step"), "prologue_check");
  EXPECT_EQ(record.at("outcome"), "mismatch");
  EXPECT_EQ(record.at("actual_prologue"), "405557488d6c24b04881eca8000000488bf9");
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
