// Host runtime-load/validate state machine (runtime/host/runtime_load.h/.cpp).
// ClassifyLoad is the pure decision core: the tests below are the mutation
// checks — they fail if the core ever reports kLoaded for a null descriptor or a
// major mismatch, or if it over-reads a short descriptor's ABI version.

#include "runtime/host/runtime_load.h"

#include "core/runtime_abi.h"
#include "runtime/host/bootstrap_diagnostics.h"
#include "runtime/host/host_identity.h"
#include "runtime/host/loader_lock.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

using nevr_host::BootstrapDiagnostics;
using nevr_host::RuntimeLoadOutcome;
using nevr_host::RuntimeLoadResult;
using nevr_host::ClassifyLoad;
using nevr_host::EmitLoadDiagnostic;
using nevr_host::ResolveRuntimePath;

std::int32_t Initialize(const NevrHostContext*) { return 0; }
std::int64_t GameMain(std::int64_t command_line) { return command_line; }

NevrRuntimeApi ValidApi() {
  NevrRuntimeApi api{};
  api.header.struct_size = sizeof(NevrRuntimeApi);
  api.header.abi_major = nevr_runtime_abi::kMajor;
  api.header.abi_minor = nevr_runtime_abi::kMinor;
  api.initialize = &Initialize;
  api.game_main = &GameMain;
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
  const int n = swprintf(dir, MAX_PATH, L"%ls\\nevr-load-%ls-%lu-%llu-%ld", base, tag,
                         static_cast<unsigned long>(GetCurrentProcessId()),
                         static_cast<unsigned long long>(GetTickCount64()),
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
                                                nevr_runtime_abi::Status::kNullDescriptor, 0);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kNoPath);
  EXPECT_FALSE(result.Loaded());
  EXPECT_EQ(result.api, nullptr);
  EXPECT_STREQ(result.DegradedAction(), "stop_game");
}

TEST(HostRuntimeLoad, DistinguishesAbsentDependencyAndGenericLoadFailures) {
  const RuntimeLoadResult absent =
      ClassifyLoad(true, L"C:\\g\\nevr-runtime.dll", kFileNotFound, nullptr, false, nullptr,
                   nevr_runtime_abi::Status::kNullDescriptor, 0);
  EXPECT_EQ(absent.outcome, RuntimeLoadOutcome::kRuntimeAbsent);
  EXPECT_EQ(absent.win32_error, kFileNotFound);

  const RuntimeLoadResult dependency =
      ClassifyLoad(true, L"C:\\g\\nevr-runtime.dll", kModuleNotFound, nullptr, false, nullptr,
                   nevr_runtime_abi::Status::kNullDescriptor, 0);
  EXPECT_EQ(dependency.outcome, RuntimeLoadOutcome::kDependencyLoadFailed);

  const RuntimeLoadResult other =
      ClassifyLoad(true, L"C:\\g\\nevr-runtime.dll", ERROR_ACCESS_DENIED, nullptr, false, nullptr,
                   nevr_runtime_abi::Status::kNullDescriptor, 0);
  EXPECT_EQ(other.outcome, RuntimeLoadOutcome::kLoadFailed);
}

TEST(HostRuntimeLoad, MissingEntryAndNullDescriptorAreNeverLoaded) {
  HMODULE module = GetModuleHandleW(nullptr);
  const RuntimeLoadResult missing =
      ClassifyLoad(true, L"C:\\g\\nevr-runtime.dll", 0, module, false, nullptr,
                   nevr_runtime_abi::Status::kNullDescriptor, 0);
  EXPECT_EQ(missing.outcome, RuntimeLoadOutcome::kMissingEntry);

  // Mutation check: a null descriptor must never be reported loaded, even when
  // the caller wrongly claims the ABI validation passed.
  const RuntimeLoadResult null_descriptor =
      ClassifyLoad(true, L"C:\\g\\nevr-runtime.dll", 0, module, true, nullptr, nevr_runtime_abi::Status::kOk, 0);
  EXPECT_EQ(null_descriptor.outcome, RuntimeLoadOutcome::kNullDescriptor);
  EXPECT_EQ(null_descriptor.api, nullptr);
}

TEST(HostRuntimeLoad, AbiMismatchIsNeverLoadedAndRecordsTheActualVersion) {
  HMODULE module = GetModuleHandleW(nullptr);
  NevrRuntimeApi api = ValidApi();
  api.header.abi_major = nevr_runtime_abi::kMajor + 1;

  const RuntimeLoadResult result = ClassifyLoad(
      true, L"C:\\g\\nevr-runtime.dll", 0, module, true, &api, nevr_runtime_abi::Status::kMajorMismatch, 0);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kAbiRejected);
  EXPECT_EQ(result.abi_status, nevr_runtime_abi::Status::kMajorMismatch);
  EXPECT_EQ(result.actual_major, nevr_runtime_abi::kMajor + 1);
  EXPECT_FALSE(result.Loaded());
  EXPECT_STREQ(result.OutcomeName(), "abi_rejected");
}

TEST(HostRuntimeLoad, InitFailureIsDistinctFromAbiRejection) {
  HMODULE module = GetModuleHandleW(nullptr);
  const NevrRuntimeApi api = ValidApi();
  const RuntimeLoadResult result = ClassifyLoad(true, L"C:\\g\\nevr-runtime.dll", 0, module, true, &api,
                                                nevr_runtime_abi::Status::kOk, 7);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kInitFailed);
  EXPECT_EQ(result.init_code, 7);
}

TEST(HostRuntimeLoad, AValidRuntimeLoadsAndDispatches) {
  HMODULE module = GetModuleHandleW(nullptr);
  const NevrRuntimeApi api = ValidApi();
  const RuntimeLoadResult result = ClassifyLoad(true, L"C:\\g\\nevr-runtime.dll", 0, module, true, &api,
                                                nevr_runtime_abi::Status::kOk, 0);
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
      true, L"C:\\g\\nevr-runtime.dll", 0, module, true, &api, nevr_runtime_abi::Status::kHeaderTooSmall, 0);
  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kAbiRejected);
  EXPECT_EQ(result.actual_major, 0u);
  EXPECT_EQ(result.actual_minor, 0u);
}

TEST(HostRuntimeLoad, EmitLoadDiagnosticWritesAParseableRecord) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", MakeTempDir(L"temp").c_str());

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare(GetModuleHandleW(nullptr));
  ASSERT_TRUE(diagnostics.PrimaryReady());

  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kAbiRejected;
  result.path_ok = true;
  const wchar_t path[] = L"C:\\games\\nevr-runtime.dll";
  std::size_t i = 0;
  while (path[i] != L'\0' && i + 1 < MAX_PATH) {
    result.path[i] = path[i];
    ++i;
  }
  result.path[i] = L'\0';
  result.abi_status = nevr_runtime_abi::Status::kMajorMismatch;
  result.actual_major = nevr_runtime_abi::kMajor + 1;
  result.actual_minor = 0;

  EmitLoadDiagnostic(diagnostics, result);

  const std::string line =
      ReadAll((local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  ASSERT_FALSE(line.empty());
  const nlohmann::json record = nlohmann::json::parse(line);
  EXPECT_EQ(record.at("event"), "runtime_load");
  EXPECT_EQ(record.at("outcome"), "abi_rejected");
  EXPECT_EQ(record.at("degraded_action"), "stop_game");
  EXPECT_EQ(record.at("path"), "C:\\games\\nevr-runtime.dll");
  EXPECT_EQ(record.at("expected_abi"), "1.1");
  EXPECT_EQ(record.at("actual_abi"), "2.0");
  EXPECT_EQ(record.at("abi_status"), "major_mismatch");
}

TEST(HostRuntimeLoad, EmitLoadDiagnosticOmitsAbiFieldsWhenNoDescriptorWasRead) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", MakeTempDir(L"temp").c_str());

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare(GetModuleHandleW(nullptr));
  ASSERT_TRUE(diagnostics.PrimaryReady());

  // A runtime that never loaded: the descriptor was never read, so abi_status
  // (which the loader initializes to kNullDescriptor before any read) and
  // actual_abi (0.0) must not be emitted — they would falsely read as "the
  // descriptor was null / ABI 0.0" when in fact the file was absent.
  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kRuntimeAbsent;
  result.path_ok = true;
  const wchar_t path[] = L"C:\\games\\nevr-runtime.dll";
  std::size_t i = 0;
  while (path[i] != L'\0' && i + 1 < MAX_PATH) {
    result.path[i] = path[i];
    ++i;
  }
  result.path[i] = L'\0';
  result.win32_error = kFileNotFound;

  EmitLoadDiagnostic(diagnostics, result);

  const std::string line =
      ReadAll((local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  ASSERT_FALSE(line.empty());
  const nlohmann::json record = nlohmann::json::parse(line);
  EXPECT_EQ(record.at("event"), "runtime_load");
  EXPECT_EQ(record.at("outcome"), "runtime_absent");
  EXPECT_EQ(record.at("degraded_action"), "stop_game");
  EXPECT_EQ(record.at("path"), "C:\\games\\nevr-runtime.dll");
  EXPECT_EQ(record.at("expected_abi"), "1.1");
  EXPECT_EQ(record.at("win32_error").get<int>(), static_cast<int>(kFileNotFound));
  EXPECT_FALSE(record.contains("abi_status"));
  EXPECT_FALSE(record.contains("actual_abi"));
}

TEST(HostRuntimeLoad, ALoadedRuntimeIsRecordedToo) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", MakeTempDir(L"temp").c_str());

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare(GetModuleHandleW(nullptr));
  const NevrRuntimeApi api = ValidApi();
  const RuntimeLoadResult result = ClassifyLoad(true, L"C:\\games\\nevr-runtime.dll", 0,
                                                GetModuleHandleW(nullptr), true, &api,
                                                nevr_runtime_abi::Status::kOk, 0);
  ASSERT_TRUE(result.Loaded());
  EmitLoadDiagnostic(diagnostics, result);

  const std::string line = ReadAll((local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  ASSERT_FALSE(line.empty());
  const nlohmann::json record = nlohmann::json::parse(line);
  EXPECT_EQ(record.at("event"), "runtime_load");
  EXPECT_EQ(record.at("outcome"), "loaded");
  EXPECT_EQ(record.at("degraded_action"), "dispatch");
  EXPECT_EQ(record.at("path"), "C:\\games\\nevr-runtime.dll");
  EXPECT_EQ(record.at("expected_abi"), "1.1");
  EXPECT_EQ(record.at("actual_abi"), "1.1");
  EXPECT_EQ(record.at("abi_status"), "ok");
  EXPECT_EQ(record.at("init_code"), 0);
  EXPECT_FALSE(record.contains("win32_error"));
}

// ADR 0012, "The root": nevr-runtime.dll resolves under %LOCALAPPDATA%\nevr\echovr, not beside BugSplat64.dll.
TEST(HostRuntimeLoad, TheRuntimeIsTheNevrRuntimeDllInTheNevrRootUnderLocalAppData) {
  wchar_t path[MAX_PATH] = {};
  const nevr_host::RuntimePathChoice choice =
      nevr_host::ResolveRuntimePathFrom(L"C:\\Users\\u\\AppData\\Local", nullptr, false, path, MAX_PATH);
  EXPECT_EQ(choice.source, nevr_host::RuntimeRootSource::kLocalAppData);
  EXPECT_FALSE(choice.override_ignored);
  EXPECT_EQ(std::wstring(path), L"C:\\Users\\u\\AppData\\Local\\nevr\\echovr\\nevr-runtime.dll");
  EXPECT_STREQ(nevr_host::RuntimeRootSourceName(choice.source), "local_appdata");
}

TEST(HostRuntimeLoad, ALocalAppDataThatEndsInASeparatorGetsNoSecondOne) {
  wchar_t path[MAX_PATH] = {};
  ASSERT_EQ(nevr_host::ResolveRuntimePathFrom(L"D:\\L\\", nullptr, false, path, MAX_PATH).source,
            nevr_host::RuntimeRootSource::kLocalAppData);
  EXPECT_EQ(std::wstring(path), L"D:\\L\\nevr\\echovr\\nevr-runtime.dll");
}

TEST(HostRuntimeLoad, TheRuntimeIsNeverFoundBesideTheHostModule) {
  wchar_t path[MAX_PATH] = {};
  ASSERT_EQ(nevr_host::ResolveRuntimePathFrom(L"C:\\Users\\u\\AppData\\Local", nullptr, false, path, MAX_PATH).source,
            nevr_host::RuntimeRootSource::kLocalAppData);
  wchar_t exe[MAX_PATH] = {};
  ASSERT_GT(GetModuleFileNameW(nullptr, exe, MAX_PATH), 0u);
  std::wstring exe_dir(exe);
  exe_dir.resize(exe_dir.find_last_of(L'\\') + 1);
  EXPECT_NE(std::wstring(path).rfind(exe_dir, 0), 0u) << "the path is not in the host's own folder";
}

TEST(HostRuntimeLoad, NevrRootReplacesTheRootInTheDeveloperBuildOnly) {
  wchar_t path[MAX_PATH] = {};
  const nevr_host::RuntimePathChoice dev =
      nevr_host::ResolveRuntimePathFrom(L"C:\\L", L"D:\\dev\\root", true, path, MAX_PATH);
  EXPECT_EQ(dev.source, nevr_host::RuntimeRootSource::kOverride);
  EXPECT_FALSE(dev.override_ignored);
  EXPECT_EQ(std::wstring(path), L"D:\\dev\\root\\nevr-runtime.dll");
  EXPECT_STREQ(nevr_host::RuntimeRootSourceName(dev.source), "nevr_root_override");

  const nevr_host::RuntimePathChoice release =
      nevr_host::ResolveRuntimePathFrom(L"C:\\L", L"D:\\dev\\root", false, path, MAX_PATH);
  EXPECT_EQ(release.source, nevr_host::RuntimeRootSource::kLocalAppData) << "a release build ignores NEVR_ROOT";
  EXPECT_TRUE(release.override_ignored) << "and says it did";
  EXPECT_EQ(std::wstring(path), L"C:\\L\\nevr\\echovr\\nevr-runtime.dll");
}

TEST(HostRuntimeLoad, ARelativeNevrRootIsIgnoredEvenInTheDeveloperBuild) {
  wchar_t path[MAX_PATH] = {};
  const nevr_host::RuntimePathChoice choice =
      nevr_host::ResolveRuntimePathFrom(L"C:\\L", L"dev\\root", true, path, MAX_PATH);
  EXPECT_EQ(choice.source, nevr_host::RuntimeRootSource::kLocalAppData);
  EXPECT_TRUE(choice.override_ignored) << "a path that depends on the working directory is not a root";
  EXPECT_EQ(std::wstring(path), L"C:\\L\\nevr\\echovr\\nevr-runtime.dll");
}

TEST(HostRuntimeLoad, NoRootMeansNoPathAndAnEmptyBuffer) {
  wchar_t path[MAX_PATH] = {L'x'};
  EXPECT_EQ(nevr_host::ResolveRuntimePathFrom(nullptr, nullptr, false, path, MAX_PATH).source,
            nevr_host::RuntimeRootSource::kUnresolved);
  EXPECT_EQ(path[0], L'\0');
  path[0] = L'x';
  EXPECT_EQ(nevr_host::ResolveRuntimePathFrom(L"", L"", true, path, MAX_PATH).source,
            nevr_host::RuntimeRootSource::kUnresolved);
  EXPECT_EQ(path[0], L'\0');
  EXPECT_STREQ(nevr_host::RuntimeRootSourceName(nevr_host::RuntimeRootSource::kUnresolved), "unresolved");
}

// The production resolver reads this process's environment and the build's own dev flag.
TEST(HostRuntimeLoad, TheProductionResolverReadsLocalAppDataAndNevrRoot) {
  wchar_t saved_local[MAX_PATH] = {};
  wchar_t saved_root[MAX_PATH] = {};
  const bool had_local = GetEnvironmentVariableW(L"LOCALAPPDATA", saved_local, MAX_PATH) != 0;
  const bool had_root = GetEnvironmentVariableW(L"NEVR_ROOT", saved_root, MAX_PATH) != 0;
  ASSERT_TRUE(SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\e\\AppData\\Local"));
  ASSERT_TRUE(SetEnvironmentVariableW(L"NEVR_ROOT", L"E:\\harness"));
  wchar_t path[MAX_PATH] = {};

  nevr_host::SetDevBuildForTest(0);
  nevr_host::RuntimePathChoice release = nevr_host::ResolveRuntimePath(path, MAX_PATH);
  const std::wstring release_path = path;
  nevr_host::SetDevBuildForTest(1);
  nevr_host::RuntimePathChoice dev = nevr_host::ResolveRuntimePath(path, MAX_PATH);
  const std::wstring dev_path = path;
  nevr_host::SetDevBuildForTest(-1);

  SetEnvironmentVariableW(L"LOCALAPPDATA", had_local ? saved_local : nullptr);
  SetEnvironmentVariableW(L"NEVR_ROOT", had_root ? saved_root : nullptr);

  EXPECT_EQ(release.source, nevr_host::RuntimeRootSource::kLocalAppData);
  EXPECT_TRUE(release.override_ignored);
  EXPECT_EQ(release_path, L"C:\\Users\\e\\AppData\\Local\\nevr\\echovr\\nevr-runtime.dll");
  EXPECT_EQ(dev.source, nevr_host::RuntimeRootSource::kOverride);
  EXPECT_EQ(dev_path, L"E:\\harness\\nevr-runtime.dll");
}

// The buffer arithmetic sizes the name from the file name itself: a buffer of exactly the root plus the name and its NUL
// fits, and one wide character fewer yields no path, an empty buffer and no write past the size it was given.
TEST(HostRuntimeLoad, ABufferOfExactlyTheRootAndTheNameFitsAndOneFewerDoesNot) {
  const std::wstring expected = L"C:\\L\\nevr\\echovr\\nevr-runtime.dll";
  const std::size_t exact = expected.size() + 1;
  constexpr wchar_t kCanary = L'\x7e7e';
  constexpr std::size_t kSpare = 8;

  std::wstring fits(exact + kSpare, kCanary);
  ASSERT_EQ(nevr_host::ResolveRuntimePathFrom(L"C:\\L", nullptr, false, fits.data(), exact).source,
            nevr_host::RuntimeRootSource::kLocalAppData);
  EXPECT_EQ(std::wstring(fits.c_str()), expected);
  for (std::size_t i = exact; i < fits.size(); ++i) EXPECT_EQ(fits[i], kCanary) << "wrote past the size at " << i;

  std::wstring short_by_one(exact + kSpare, kCanary);
  EXPECT_EQ(nevr_host::ResolveRuntimePathFrom(L"C:\\L", nullptr, false, short_by_one.data(), exact - 1).source,
            nevr_host::RuntimeRootSource::kUnresolved);
  EXPECT_EQ(short_by_one[0], L'\0');
  for (std::size_t i = exact - 1; i < short_by_one.size(); ++i) {
    EXPECT_EQ(short_by_one[i], kCanary) << "wrote past the size at " << i;
  }

  std::wstring override_fits(32, kCanary);
  const std::wstring override_expected = L"D:\\r\\nevr-runtime.dll";
  EXPECT_EQ(nevr_host::ResolveRuntimePathFrom(L"C:\\L", L"D:\\r", true, override_fits.data(), override_expected.size() + 1).source,
            nevr_host::RuntimeRootSource::kOverride);
  EXPECT_EQ(std::wstring(override_fits.c_str()), override_expected);
  std::wstring override_short(32, kCanary);
  EXPECT_EQ(nevr_host::ResolveRuntimePathFrom(L"C:\\L", L"D:\\r", true, override_short.data(), override_expected.size()).source,
            nevr_host::RuntimeRootSource::kUnresolved);
  EXPECT_EQ(override_short[0], L'\0');
}

TEST(HostRuntimeLoad, APathThatDoesNotFitYieldsNoPathAndAnEmptyBuffer) {
  wchar_t path[8] = {L'x'};
  EXPECT_EQ(nevr_host::ResolveRuntimePathFrom(L"C:\\L", nullptr, false, path, 8).source,
            nevr_host::RuntimeRootSource::kUnresolved);
  EXPECT_EQ(path[0], L'\0');
  EXPECT_EQ(nevr_host::ResolveRuntimePathFrom(L"C:\\L", nullptr, false, nullptr, 8).source,
            nevr_host::RuntimeRootSource::kUnresolved);
  EXPECT_EQ(nevr_host::ResolveRuntimePathFrom(L"C:\\L", nullptr, false, path, 0).source,
            nevr_host::RuntimeRootSource::kUnresolved);
}

// The runtime_load record says which root the runtime was looked for in, and when a NEVR_ROOT that was set was not honoured.
TEST(HostRuntimeLoad, TheLoadRecordNamesTheRootAndSaysWhenNevrRootWasIgnored) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", MakeTempDir(L"temp").c_str());
  const std::wstring log = local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl";

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare(GetModuleHandleW(nullptr));
  ASSERT_TRUE(diagnostics.PrimaryReady());

  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kRuntimeAbsent;
  result.path_ok = true;
  const wchar_t path[] = L"C:\\L\\nevr\\echovr\\nevr-runtime.dll";
  std::memcpy(result.path, path, sizeof(path));
  result.root_source = nevr_host::RuntimeRootSource::kLocalAppData;
  result.root_override_ignored = true;
  EmitLoadDiagnostic(diagnostics, result);
  const nlohmann::json ignored = nlohmann::json::parse(ReadAll(log.c_str()));
  EXPECT_EQ(ignored.at("root"), "local_appdata");
  EXPECT_EQ(ignored.at("root_override"), "ignored");
  EXPECT_EQ(ignored.at("path"), "C:\\L\\nevr\\echovr\\nevr-runtime.dll");

  DeleteFileW(log.c_str());
  result.root_override_ignored = false;
  result.root_source = nevr_host::RuntimeRootSource::kOverride;
  EmitLoadDiagnostic(diagnostics, result);
  const nlohmann::json plain = nlohmann::json::parse(ReadAll(log.c_str()));
  EXPECT_EQ(plain.at("root"), "nevr_root_override");
  EXPECT_FALSE(plain.contains("root_override"));
}

// --- loader_lock_held: the PEB loader-lock owner, measured by the host itself ---------------

namespace {

using LdrLockFn = LONG(NTAPI*)(ULONG flags, ULONG* disposition, PVOID* cookie);
using LdrUnlockFn = LONG(NTAPI*)(ULONG flags, PVOID cookie);

// Holds the process loader lock on this thread for the scope, the way DllMain's callers do.
class ScopedLoaderLock {
 public:
  ScopedLoaderLock() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    lock_ = reinterpret_cast<LdrLockFn>(GetProcAddress(ntdll, "LdrLockLoaderLock"));
    unlock_ = reinterpret_cast<LdrUnlockFn>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"));
    if (lock_ != nullptr && unlock_ != nullptr) {
      ULONG disposition = 0;
      held_ = lock_(0, &disposition, &cookie_) >= 0 && disposition == 1;
    }
  }
  ~ScopedLoaderLock() {
    if (held_) unlock_(0, cookie_);
  }
  bool Held() const { return held_; }

 private:
  LdrLockFn lock_ = nullptr;
  LdrUnlockFn unlock_ = nullptr;
  PVOID cookie_ = nullptr;
  bool held_ = false;
};

}  // namespace

// Nothing is asserted (and so nothing printed) while the lock is held: a failure message written
// under the loader lock can hang the test instead of failing it. Facts are read inside the
// scope and judged after it.
TEST(HostLoaderLock, IsFalseOutsideTheLockAndTrueWhileThisThreadHoldsIt) {
  const bool before = nevr_host::LoaderLockHeldByThisThread();
  bool acquired = false;
  bool inside = false;
  {
    ScopedLoaderLock lock;
    acquired = lock.Held();
    inside = nevr_host::LoaderLockHeldByThisThread();
  }
  const bool after = nevr_host::LoaderLockHeldByThisThread();
  EXPECT_FALSE(before);
  ASSERT_TRUE(acquired);
  EXPECT_TRUE(inside);
  EXPECT_FALSE(after);
}

// A thread cannot start while the loader lock is held (thread attach takes it), so the other
// thread is started first and parked on an event; it answers through a second event, because
// its exit would also wait for the lock.
namespace {

struct OtherThread {
  HANDLE ready = nullptr;
  HANDLE go = nullptr;
  HANDLE answered = nullptr;
  bool held = true;
};

DWORD WINAPI AskFromOtherThread(void* raw) {
  auto* state = static_cast<OtherThread*>(raw);
  SetEvent(state->ready);  // past thread attach: the loader lock may be taken now
  WaitForSingleObject(state->go, 10000);
  state->held = nevr_host::LoaderLockHeldByThisThread();
  SetEvent(state->answered);
  return 0;
}

}  // namespace

TEST(HostLoaderLock, AnotherThreadHoldingTheLockIsNotThisThreadHoldingIt) {
  OtherThread state;
  state.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  state.go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  state.answered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(state.go, nullptr);
  ASSERT_NE(state.answered, nullptr);
  HANDLE thread = CreateThread(nullptr, 0, &AskFromOtherThread, &state, 0, nullptr);
  ASSERT_NE(thread, nullptr);
  ASSERT_EQ(WaitForSingleObject(state.ready, 10000), WAIT_OBJECT_0);
  bool acquired = false;
  bool mine = false;
  DWORD answered = WAIT_FAILED;
  {
    ScopedLoaderLock lock;
    acquired = lock.Held();
    mine = nevr_host::LoaderLockHeldByThisThread();
    SetEvent(state.go);
    answered = WaitForSingleObject(state.answered, 10000);
  }
  EXPECT_TRUE(acquired);
  EXPECT_TRUE(mine);
  EXPECT_EQ(answered, WAIT_OBJECT_0);
  EXPECT_FALSE(state.held);  // this thread owned the lock, the other one did not
  WaitForSingleObject(thread, 10000);
  CloseHandle(thread);
  CloseHandle(state.ready);
  CloseHandle(state.go);
  CloseHandle(state.answered);
}

// The runtime_load record says whether the runtime was loaded under the loader lock, so a
// log shows the rendezvous ran outside it. Measured by Load itself, not passed in.
TEST(HostRuntimeLoad, LoadRecordsWhetherTheLoaderLockWasHeld) {
  // A folder with no nevr-runtime.dll: the load ends at "runtime absent" without a LoadLibrary.
  static const std::wstring empty_root = MakeTempDir(L"empty-root");
  nevr_host::SetRuntimeDirForTest(empty_root.c_str());
  nevr_host::RuntimeLoader loader;
  loader.Prepare();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(L"", L"", L"");
  NevrHostContext host{};

  const RuntimeLoadResult outside = loader.Load(host, diagnostics);

  RuntimeLoadResult inside;
  bool acquired = false;
  {
    ScopedLoaderLock lock;
    acquired = lock.Held();
    inside = loader.Load(host, diagnostics);
  }
  nevr_host::SetRuntimeDirForTest(nullptr);
  EXPECT_EQ(outside.outcome, RuntimeLoadOutcome::kRuntimeAbsent);
  EXPECT_FALSE(outside.loader_lock_held);
  ASSERT_TRUE(acquired);
  EXPECT_EQ(inside.outcome, RuntimeLoadOutcome::kRuntimeAbsent);
  EXPECT_TRUE(inside.loader_lock_held);
}

// Prepare and Load run the production rule end to end: the file looked for is in the nEVR root of this process's
// %LOCALAPPDATA%, a NEVR_ROOT that is set is not honoured by a release build, and the result says both.
TEST(HostRuntimeLoad, LoadLooksInTheNevrRootAndTheResultSaysWhichRootAndWhetherNevrRootWasIgnored) {
  wchar_t saved_local[MAX_PATH] = {};
  wchar_t saved_root[MAX_PATH] = {};
  const bool had_local = GetEnvironmentVariableW(L"LOCALAPPDATA", saved_local, MAX_PATH) != 0;
  const bool had_root = GetEnvironmentVariableW(L"NEVR_ROOT", saved_root, MAX_PATH) != 0;
  const std::wstring local = MakeTempDir(L"local-root");
  const std::wstring elsewhere = MakeTempDir(L"nevr-root-elsewhere");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"NEVR_ROOT", elsewhere.c_str());
  nevr_host::SetRuntimeDirForTest(nullptr);
  nevr_host::SetDevBuildForTest(0);

  nevr_host::RuntimeLoader loader;
  loader.Prepare();
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(L"", L"", L"");
  NevrHostContext host{};
  const RuntimeLoadResult result = loader.Load(host, diagnostics);

  nevr_host::SetDevBuildForTest(-1);
  SetEnvironmentVariableW(L"LOCALAPPDATA", had_local ? saved_local : nullptr);
  SetEnvironmentVariableW(L"NEVR_ROOT", had_root ? saved_root : nullptr);

  EXPECT_EQ(result.outcome, RuntimeLoadOutcome::kRuntimeAbsent);
  EXPECT_TRUE(result.path_ok);
  EXPECT_EQ(std::wstring(result.path), local + L"\\nevr\\echovr\\nevr-runtime.dll");
  EXPECT_EQ(result.root_source, nevr_host::RuntimeRootSource::kLocalAppData);
  EXPECT_TRUE(result.root_override_ignored);
}

TEST(HostRuntimeLoad, TheLoadRecordCarriesLoaderLockHeld) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);

  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kRuntimeAbsent;
  result.loader_lock_held = false;
  EmitLoadDiagnostic(diagnostics, result);
  result.loader_lock_held = true;
  EmitLoadDiagnostic(diagnostics, result);

  const std::string text = ReadAll((local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  const std::size_t split = text.find('\n');
  ASSERT_NE(split, std::string::npos);
  const nlohmann::json first = nlohmann::json::parse(text.substr(0, split));
  const nlohmann::json second = nlohmann::json::parse(text.substr(split + 1));
  EXPECT_EQ(first.at("loader_lock_held"), false);
  EXPECT_EQ(second.at("loader_lock_held"), true);
}

// EmitLoadDiagnostic runs inside an InitOnce callback under the game's frames: a record that cannot be
// built (allocation, a bad string) must come out as a fixed-buffer marker line, never as an exception.
TEST(HostRuntimeLoad, ARecordThatCannotBeBuiltBecomesAMarkerLineNotAnException) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kAbiRejected;
  result.loader_lock_held = true;

  nevr_host::SetEmitLoadFailureForTest(true);
  EXPECT_NO_THROW(EmitLoadDiagnostic(diagnostics, result));
  nevr_host::SetEmitLoadFailureForTest(false);

  const std::string text = ReadAll((local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  ASSERT_FALSE(text.empty());
  const nlohmann::json marker = nlohmann::json::parse(text.substr(0, text.find('\n')));
  EXPECT_EQ(marker.at("tag"), "NEVR.HOST");
  EXPECT_EQ(marker.at("event"), "runtime_load");
  EXPECT_EQ(marker.at("error"), "record_build_failed");
  EXPECT_EQ(marker.at("outcome"), "abi_rejected");
  EXPECT_EQ(marker.at("loader_lock_held"), true);
  EXPECT_TRUE(marker.contains("what"));
}

// The host stops the game when the runtime did not load (docs/adr/0012-static-install-root.md): the exit code is
// 78, EX_CONFIG, and the record that says so names the cause, the code, the host's identity and the path searched.
TEST(HostRuntimeLoad, TheStopExitCodeIs78) {
  EXPECT_EQ(nevr_host::kRuntimeStopExitCode, 78u);
}

TEST(HostRuntimeLoad, EveryOutcomeButLoadedNamesTheStopAsItsAction) {
  RuntimeLoadResult result;
  for (const RuntimeLoadOutcome outcome :
       {RuntimeLoadOutcome::kNoPath, RuntimeLoadOutcome::kRuntimeAbsent, RuntimeLoadOutcome::kDependencyLoadFailed,
        RuntimeLoadOutcome::kLoadFailed, RuntimeLoadOutcome::kMissingEntry, RuntimeLoadOutcome::kNullDescriptor,
        RuntimeLoadOutcome::kAbiRejected, RuntimeLoadOutcome::kInitFailed}) {
    result.outcome = outcome;
    EXPECT_STREQ(result.DegradedAction(), "stop_game") << result.OutcomeName();
  }
  result.outcome = RuntimeLoadOutcome::kLoaded;
  EXPECT_STREQ(result.DegradedAction(), "dispatch");
}

TEST(HostRuntimeLoad, TheStopRecordNamesTheCauseTheCodeTheHostAndThePathSearched) {
  const std::wstring local = MakeTempDir(L"stop");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kRuntimeAbsent;
  result.path_ok = true;
  result.win32_error = ERROR_FILE_NOT_FOUND;
  const wchar_t path[] = L"C:\\games\\echovr\\bin\\win10\\nevr-runtime.dll";
  std::size_t i = 0;
  for (; path[i] != L'\0'; ++i) result.path[i] = path[i];
  result.path[i] = L'\0';

  nevr_host::EmitStopDiagnostic(diagnostics, result);

  const std::string text = ReadAll((local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  ASSERT_FALSE(text.empty());
  const nlohmann::json record = nlohmann::json::parse(text.substr(0, text.find('\n')));
  EXPECT_EQ(record.at("tag"), "NEVR.HOST");
  EXPECT_EQ(record.at("event"), "game_stop");
  EXPECT_EQ(record.at("reason"), "runtime_absent");
  EXPECT_EQ(record.at("exit_code"), 78);
  EXPECT_EQ(record.at("runtime_path"), "C:\\games\\echovr\\bin\\win10\\nevr-runtime.dll");
  EXPECT_EQ(record.at("win32_error"), ERROR_FILE_NOT_FOUND);
  // The host's identity: the literal the packager reads from this file, so the log names exactly what ran.
  EXPECT_EQ(record.at("host_identity"), std::string(nevr_host::IdentityLiteral()));
  EXPECT_EQ(std::string(nevr_host::IdentityLiteral()).rfind("NEVR-BUILD ", 0), 0u);
}

TEST(HostRuntimeLoad, AStopRecordThatCannotBeBuiltBecomesAMarkerLineNotAnException) {
  const std::wstring local = MakeTempDir(L"stopmark");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  RuntimeLoadResult result;
  result.outcome = RuntimeLoadOutcome::kInitFailed;

  nevr_host::SetEmitLoadFailureForTest(true);
  EXPECT_NO_THROW(nevr_host::EmitStopDiagnostic(diagnostics, result));
  nevr_host::SetEmitLoadFailureForTest(false);

  const std::string text = ReadAll((local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl").c_str());
  ASSERT_FALSE(text.empty());
  const nlohmann::json marker = nlohmann::json::parse(text.substr(0, text.find('\n')));
  EXPECT_EQ(marker.at("event"), "game_stop");
  EXPECT_EQ(marker.at("error"), "record_build_failed");
  EXPECT_EQ(marker.at("reason"), "init_failed");
  EXPECT_EQ(marker.at("exit_code"), 78);
}

// An exception that escapes the runtime's initialize() is a load failure, not a crash in the host's detour.
namespace {
std::int32_t ThrowingInitialize(const NevrHostContext*) { throw std::runtime_error("initialize threw"); }
std::int32_t FailingInitialize(const NevrHostContext*) { return 7; }
// Not derived from std::exception: a runtime built with another library, or a throw of a plain value.
struct ForeignError {};
std::int32_t ThrowingForeignInitialize(const NevrHostContext*) { throw ForeignError{}; }
std::int32_t ThrowingIntInitialize(const NevrHostContext*) { throw 42; }
}  // namespace

TEST(HostRuntimeLoad, AnExceptionFromInitializeIsCaughtAndReportedAsAFailure) {
  NevrRuntimeApi api = ValidApi();
  api.initialize = &ThrowingInitialize;
  NevrHostContext host{};
  bool threw = false;
  std::int32_t code = 0;
  EXPECT_NO_THROW(code = nevr_host::RuntimeLoader::CallInitialize(&api, host, &threw));
  EXPECT_TRUE(threw);
  EXPECT_EQ(code, nevr_host::kInitThrewCode);
  // ... and the classifier turns that nonzero code into the init_failed outcome the stop acts on.
  const RuntimeLoadResult classified = ClassifyLoad(true, L"x", 0, GetModuleHandleW(nullptr), true, &api,
                                                    nevr_runtime_abi::Status::kOk, code);
  EXPECT_EQ(classified.outcome, RuntimeLoadOutcome::kInitFailed);
}

// Nothing crosses the DLL boundary as an exception, whatever type it is (the ellipsis catch in CallInitialize).
TEST(HostRuntimeLoad, AnExceptionOfAnyTypeFromInitializeIsCaughtAndReportedAsAFailure) {
  NevrHostContext host{};
  for (auto initialize : {&ThrowingForeignInitialize, &ThrowingIntInitialize}) {
    NevrRuntimeApi api = ValidApi();
    api.initialize = initialize;
    bool threw = false;
    std::int32_t code = 0;
    EXPECT_NO_THROW(code = nevr_host::RuntimeLoader::CallInitialize(&api, host, &threw));
    EXPECT_TRUE(threw);
    EXPECT_EQ(code, nevr_host::kInitThrewCode);
  }
}

TEST(HostRuntimeLoad, AnInitializeThatReturnsNormallyIsNotReportedAsThrowing) {
  NevrRuntimeApi api = ValidApi();
  NevrHostContext host{};
  bool threw = true;
  EXPECT_EQ(nevr_host::RuntimeLoader::CallInitialize(&api, host, &threw), 0);
  EXPECT_FALSE(threw);
  api.initialize = &FailingInitialize;
  EXPECT_EQ(nevr_host::RuntimeLoader::CallInitialize(&api, host, &threw), 7);
  EXPECT_FALSE(threw);
}
