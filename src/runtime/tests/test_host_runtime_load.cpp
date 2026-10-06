// Host runtime-load/validate state machine (runtime/host/runtime_load.h/.cpp).
// ClassifyLoad is the pure decision core: the tests below are the mutation
// checks — they fail if the core ever reports kLoaded for a null descriptor or a
// major mismatch, or if it over-reads a short descriptor's ABI version.

#include "runtime/host/runtime_load.h"

#include "core/runtime_abi.h"
#include "runtime/host/bootstrap_diagnostics.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using NevrHost::BootstrapDiagnostics;
using NevrHost::RuntimeLoadOutcome;
using NevrHost::RuntimeLoadResult;
using NevrHost::ClassifyLoad;
using NevrHost::EmitLoadDiagnostic;

std::int32_t Initialize(const NevrHostContext*) { return 0; }
void Before(void*) {}
void After(void*, std::uint64_t) {}

NevrRuntimeApi ValidApi() {
  NevrRuntimeApi api{};
  api.header.struct_size = sizeof(NevrRuntimeApi);
  api.header.abi_major = NevrAbi::kMajor;
  api.header.abi_minor = NevrAbi::kMinor;
  api.initialize = &Initialize;
  api.preprocess_before = &Before;
  api.preprocess_after = &After;
  return api;
}

constexpr DWORD kFileNotFound = ERROR_FILE_NOT_FOUND;
constexpr DWORD kModuleNotFound = ERROR_MOD_NOT_FOUND;

std::wstring MakeTempDir(const wchar_t* tag) {
  wchar_t base[MAX_PATH] = {};
  const DWORD base_len = GetTempPathW(MAX_PATH, base);
  EXPECT_GT(base_len, 0u);
  EXPECT_LT(base_len, static_cast<DWORD>(MAX_PATH));
  static LONG counter = 0;
  const LONG sequence = InterlockedIncrement(&counter);
  wchar_t dir[MAX_PATH] = {};
  const int n = swprintf(dir, MAX_PATH, L"%ls\\nevr-load-%ls-%lu-%ld", base, tag,
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

}  // namespace

TEST(HostRuntimeLoad, NoPathYieldsNoPath) {
  const RuntimeLoadResult result = ClassifyLoad(false, L"", 0, nullptr, false, nullptr,
                                                NevrAbi::Status::kNullDescriptor, 0);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kNoPath);
  EXPECT_FALSE(result.Loaded());
  EXPECT_EQ(result.api, nullptr);
  EXPECT_STREQ(result.DegradedAction(), "run_original_only");
}

TEST(HostRuntimeLoad, DistinguishesAbsentDependencyAndGenericLoadFailures) {
  const RuntimeLoadResult absent =
      ClassifyLoad(true, L"C:\\g\\nevr.dll", kFileNotFound, nullptr, false, nullptr,
                   NevrAbi::Status::kNullDescriptor, 0);
  EXPECT_EQ(absent.outcome, RuntimeLoadOutcome::kRuntimeAbsent);
  EXPECT_EQ(absent.win32_error, kFileNotFound);

  const RuntimeLoadResult dependency =
      ClassifyLoad(true, L"C:\\g\\nevr.dll", kModuleNotFound, nullptr, false, nullptr,
                   NevrAbi::Status::kNullDescriptor, 0);
  EXPECT_EQ(dependency.outcome, RuntimeLoadOutcome::kDependencyLoadFailed);

  const RuntimeLoadResult other =
      ClassifyLoad(true, L"C:\\g\\nevr.dll", ERROR_ACCESS_DENIED, nullptr, false, nullptr,
                   NevrAbi::Status::kNullDescriptor, 0);
  EXPECT_EQ(other.outcome, RuntimeLoadOutcome::kLoadFailed);
}

TEST(HostRuntimeLoad, MissingEntryAndNullDescriptorAreNeverLoaded) {
  HMODULE module = GetModuleHandleW(nullptr);
  const RuntimeLoadResult missing =
      ClassifyLoad(true, L"C:\\g\\nevr.dll", 0, module, false, nullptr,
                   NevrAbi::Status::kNullDescriptor, 0);
  EXPECT_EQ(missing.outcome, RuntimeLoadOutcome::kMissingEntry);

  // Mutation check: a null descriptor must never be reported loaded, even when
  // the caller wrongly claims the ABI validation passed.
  const RuntimeLoadResult null_descriptor =
      ClassifyLoad(true, L"C:\\g\\nevr.dll", 0, module, true, nullptr, NevrAbi::Status::kOk, 0);
  EXPECT_EQ(null_descriptor.outcome, RuntimeLoadOutcome::kNullDescriptor);
  EXPECT_EQ(null_descriptor.api, nullptr);
}

TEST(HostRuntimeLoad, AbiMismatchIsNeverLoadedAndRecordsTheActualVersion) {
  HMODULE module = GetModuleHandleW(nullptr);
  NevrRuntimeApi api = ValidApi();
  api.header.abi_major = NevrAbi::kMajor + 1;

  const RuntimeLoadResult result = ClassifyLoad(
      true, L"C:\\g\\nevr.dll", 0, module, true, &api, NevrAbi::Status::kMajorMismatch, 0);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kAbiRejected);
  EXPECT_EQ(result.abi_status, NevrAbi::Status::kMajorMismatch);
  EXPECT_EQ(result.actual_major, NevrAbi::kMajor + 1);
  EXPECT_FALSE(result.Loaded());
  EXPECT_STREQ(result.OutcomeName(), "abi_rejected");
}

TEST(HostRuntimeLoad, InitFailureIsDistinctFromAbiRejection) {
  HMODULE module = GetModuleHandleW(nullptr);
  const NevrRuntimeApi api = ValidApi();
  const RuntimeLoadResult result = ClassifyLoad(true, L"C:\\g\\nevr.dll", 0, module, true, &api,
                                                NevrAbi::Status::kOk, 7);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kInitFailed);
  EXPECT_EQ(result.init_code, 7);
}

TEST(HostRuntimeLoad, AValidRuntimeLoadsAndDispatches) {
  HMODULE module = GetModuleHandleW(nullptr);
  const NevrRuntimeApi api = ValidApi();
  const RuntimeLoadResult result = ClassifyLoad(true, L"C:\\g\\nevr.dll", 0, module, true, &api,
                                                NevrAbi::Status::kOk, 0);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kLoaded);
  EXPECT_TRUE(result.Loaded());
  EXPECT_EQ(result.api, &api);
  EXPECT_STREQ(result.DegradedAction(), "dispatch");
}

TEST(HostRuntimeLoad, DoesNotReadTheActualVersionPastAShortHeader) {
  HMODULE module = GetModuleHandleW(nullptr);
  // A truthful pre-v1 writer: struct_size == 16 (header only). ClassifyLoad must
  // not read abi_major/abi_minor from it.
  NevrRuntimeApi api = ValidApi();
  api.header.struct_size = sizeof(NevrAbiHeader);
  const RuntimeLoadResult result = ClassifyLoad(
      true, L"C:\\g\\nevr.dll", 0, module, true, &api, NevrAbi::Status::kHeaderTooSmall, 0);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kAbiRejected);
  EXPECT_EQ(result.actual_major, 0u);
  EXPECT_EQ(result.actual_minor, 0u);
}

TEST(HostRuntimeLoad, EmitLoadDiagnosticWritesAParseableRecord) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", MakeTempDir(L"temp").c_str());

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  ASSERT_TRUE(diagnostics.PrimaryReady());

  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kAbiRejected;
  result.path_ok = true;
  const wchar_t path[] = L"C:\\games\\nevr.dll";
  std::size_t i = 0;
  while (path[i] != L'\0' && i + 1 < MAX_PATH) {
    result.path[i] = path[i];
    ++i;
  }
  result.path[i] = L'\0';
  result.abi_status = NevrAbi::Status::kMajorMismatch;
  result.actual_major = NevrAbi::kMajor + 1;
  result.actual_minor = 0;

  EmitLoadDiagnostic(diagnostics, result);

  const std::string line =
      ReadAll((local + L"\\EchoVR\\logs\\nevr-bootstrap.jsonl").c_str());
  ASSERT_FALSE(line.empty());
  const nlohmann::json record = nlohmann::json::parse(line);
  EXPECT_EQ(record.at("event"), "runtime_load");
  EXPECT_EQ(record.at("outcome"), "abi_rejected");
  EXPECT_EQ(record.at("degraded_action"), "run_original_only");
  EXPECT_EQ(record.at("path"), "C:\\games\\nevr.dll");
  EXPECT_EQ(record.at("expected_abi"), "1.0");
  EXPECT_EQ(record.at("actual_abi"), "2.0");
  EXPECT_EQ(record.at("abi_status"), "major_mismatch");
}

TEST(HostRuntimeLoad, EmitLoadDiagnosticOmitsAbiFieldsWhenNoDescriptorWasRead) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", MakeTempDir(L"temp").c_str());

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare();
  ASSERT_TRUE(diagnostics.PrimaryReady());

  // A runtime that never loaded: the descriptor was never read, so abi_status
  // (which the loader initializes to kNullDescriptor before any read) and
  // actual_abi (0.0) must not be emitted — they would falsely read as "the
  // descriptor was null / ABI 0.0" when in fact the file was absent.
  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kRuntimeAbsent;
  result.path_ok = true;
  const wchar_t path[] = L"C:\\games\\nevr.dll";
  std::size_t i = 0;
  while (path[i] != L'\0' && i + 1 < MAX_PATH) {
    result.path[i] = path[i];
    ++i;
  }
  result.path[i] = L'\0';
  result.win32_error = kFileNotFound;

  EmitLoadDiagnostic(diagnostics, result);

  const std::string line =
      ReadAll((local + L"\\EchoVR\\logs\\nevr-bootstrap.jsonl").c_str());
  ASSERT_FALSE(line.empty());
  const nlohmann::json record = nlohmann::json::parse(line);
  EXPECT_EQ(record.at("event"), "runtime_load");
  EXPECT_EQ(record.at("outcome"), "runtime_absent");
  EXPECT_EQ(record.at("degraded_action"), "run_original_only");
  EXPECT_EQ(record.at("path"), "C:\\games\\nevr.dll");
  EXPECT_EQ(record.at("expected_abi"), "1.0");
  EXPECT_EQ(record.at("win32_error").get<int>(), static_cast<int>(kFileNotFound));
  EXPECT_FALSE(record.contains("abi_status"));
  EXPECT_FALSE(record.contains("actual_abi"));
}
