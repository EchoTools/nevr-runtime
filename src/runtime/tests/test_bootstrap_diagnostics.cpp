// Host bootstrap diagnostics (runtime/host/bootstrap_diagnostics.h/.cpp): the
// fixed-buffer, one-bounded-WriteFile JSONL recorder the stable host uses for
// bootstrap/crash records.

#include "runtime/host/bootstrap_diagnostics.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

using NevrHost::BootstrapDiagnostics;
using NevrHost::DiagnosticDestination;

// A freshly created unique directory under %TEMP%. Must be called before any
// test overwrites the TEMP environment variable.
std::wstring MakeTempDir(const wchar_t* tag) {
  wchar_t base[MAX_PATH] = {};
  const DWORD base_len = GetTempPathW(MAX_PATH, base);
  EXPECT_GT(base_len, 0u);
  EXPECT_LT(base_len, static_cast<DWORD>(MAX_PATH));

  static LONG counter = 0;
  const LONG sequence = InterlockedIncrement(&counter);
  wchar_t dir[MAX_PATH] = {};
  const int n = swprintf(dir, MAX_PATH, L"%ls\\nevr-diag-%ls-%lu-%ld", base, tag,
                         static_cast<unsigned long>(GetCurrentProcessId()),
                         static_cast<long>(sequence));
  EXPECT_GT(n, 0);
  EXPECT_NE(CreateDirectoryW(dir, nullptr), FALSE);
  return std::wstring(dir);
}

std::string ReadAll(const wchar_t* path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
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

void SetEnv(const wchar_t* name, const std::wstring& value) {
  SetEnvironmentVariableW(name, value.c_str());
}

const std::wstring kLogsFile = L"\\EchoVR\\logs\\nevr-bootstrap.jsonl";

}  // namespace

TEST(BootstrapDiagnostics, PrepareBuildsBothDirectoriesFromEnvironment) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  SetEnv(L"LOCALAPPDATA", local);
  SetEnv(L"TEMP", temp);

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  ASSERT_TRUE(diagnostics.PrimaryReady());
  ASSERT_TRUE(diagnostics.FallbackReady());
  EXPECT_EQ(std::wstring(diagnostics.PrimaryDir()), local + L"\\EchoVR\\logs");
  EXPECT_EQ(std::wstring(diagnostics.FallbackDir()), temp + L"\\EchoVR\\logs");
}

TEST(BootstrapDiagnostics, EmitWritesOneParseableJsonLineToPrimary) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  SetEnv(L"LOCALAPPDATA", local);
  SetEnv(L"TEMP", temp);

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  nlohmann::json record = {{"event", "runtime_load"}, {"win32_error", 2}};
  EXPECT_EQ(diagnostics.Emit(record), DiagnosticDestination::kPrimary);

  const std::string line = ReadAll((local + kLogsFile).c_str());
  ASSERT_FALSE(line.empty());
  // The record must round-trip: hand-built JSON that cannot be parsed is the
  // exact failure AGENTS.md "no hand-built serialization" forbids.
  const nlohmann::json parsed = nlohmann::json::parse(line);
  EXPECT_EQ(parsed.at("event"), "runtime_load");
  EXPECT_EQ(parsed.at("win32_error"), 2);
}

TEST(BootstrapDiagnostics, EmitFallsBackWhenPrimaryIsUnwritable) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  // Block the primary: create "EchoVR" as a FILE under %LOCALAPPDATA%, so the
  // logs directory beneath it cannot be created or written.
  const std::wstring blocker = local + L"\\EchoVR";
  HANDLE file = CreateFileW(blocker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);

  SetEnv(L"LOCALAPPDATA", local);
  SetEnv(L"TEMP", temp);

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  nlohmann::json record = {{"event", "runtime_load"}, {"win32_error", 5}};
  EXPECT_EQ(diagnostics.Emit(record), DiagnosticDestination::kFallback);

  const std::string line = ReadAll((temp + kLogsFile).c_str());
  ASSERT_FALSE(line.empty());
  EXPECT_EQ(nlohmann::json::parse(line).at("win32_error"), 5);
}

TEST(BootstrapDiagnostics, EmitDropsWhenBothDestinationsAreUnavailable) {
  SetEnv(L"LOCALAPPDATA", L"");
  SetEnv(L"TEMP", L"");

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  EXPECT_FALSE(diagnostics.PrimaryReady());
  EXPECT_FALSE(diagnostics.FallbackReady());
  EXPECT_EQ(diagnostics.Emit({{"event", "runtime_load"}}), DiagnosticDestination::kDropped);
}

TEST(BootstrapDiagnostics, EmitRefusesAnOversizedRecordRatherThanTruncate) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnv(L"LOCALAPPDATA", local);
  SetEnv(L"TEMP", MakeTempDir(L"temp"));

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  nlohmann::json record;
  record["event"] = "runtime_load";
  record["huge"] = std::string(BootstrapDiagnostics::kMaxRecordBytes, 'x');
  EXPECT_EQ(diagnostics.Emit(record), DiagnosticDestination::kDropped);
}

TEST(BootstrapDiagnostics, RecordAttachFailureAndFlushEmitsOneHostAttachRecord) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnv(L"LOCALAPPDATA", local);
  SetEnv(L"TEMP", MakeTempDir(L"temp"));

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  ASSERT_FALSE(diagnostics.AttachDiagnosticFlushed());
  diagnostics.RecordAttachFailure(NevrHost::AttachFailure::kPrologueMismatch, 0xb2);
  EXPECT_EQ(diagnostics.AttachFailureKind(), NevrHost::AttachFailure::kPrologueMismatch);
  diagnostics.FlushPendingAttachDiagnostic();

  const std::string line = ReadAll((local + kLogsFile).c_str());
  ASSERT_FALSE(line.empty());
  const nlohmann::json parsed = nlohmann::json::parse(line);
  EXPECT_EQ(parsed.at("event"), "host_attach");
  EXPECT_EQ(parsed.at("outcome"), "prologue_mismatch");
  EXPECT_EQ(parsed.at("degraded_action"), "run_original_only");
  EXPECT_EQ(parsed.at("detail"), 0xb2);
}

TEST(BootstrapDiagnostics, FlushWithoutFailureIsANoopAndFlushIsIdempotent) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnv(L"LOCALAPPDATA", local);
  SetEnv(L"TEMP", MakeTempDir(L"temp"));

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  diagnostics.FlushPendingAttachDiagnostic();  // nothing recorded: no line
  EXPECT_EQ(diagnostics.Emit({{"event", "runtime_load"}}), DiagnosticDestination::kPrimary);

  diagnostics.RecordAttachFailure(NevrHost::AttachFailure::kHookFailed);
  diagnostics.FlushPendingAttachDiagnostic();
  diagnostics.FlushPendingAttachDiagnostic();  // idempotent: no second host_attach
  EXPECT_TRUE(diagnostics.AttachDiagnosticFlushed());

  const std::string text = ReadAll((local + kLogsFile).c_str());
  const std::size_t first = text.find("\"event\":\"host_attach\"");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(text.find("\"event\":\"host_attach\"", first + 1), std::string::npos);
  EXPECT_NE(text.find("\"event\":\"runtime_load\""), std::string::npos);
}
