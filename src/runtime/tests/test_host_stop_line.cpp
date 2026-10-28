// The one console line the BugSplat64.dll host writes whenever it stops the game, or leaves it running without
// nevr-runtime.dll (runtime/host/stop_line.h, #1252). Every line a test reads back is parsed with nlohmann::json, and the
// field order is read from the text, since a parsed object does not keep it.

#include "runtime/host/stop_line.h"

#include "runtime/host/bootstrap_diagnostics.h"
#include "runtime/host/host_identity.h"
#include "runtime/host/runtime_load.h"
#include "runtime/host/stop_notice.h"
#include "runtime/tests/stderr_capture.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

using nevr_host::AttachFailure;
using nevr_host::AttachOutcome;
using nevr_host::AttachStep;
using nevr_host::BootstrapDiagnostics;
using nevr_host::HookStage;
using nevr_host::RuntimeLoadOutcome;
using nevr_host::RuntimeLoadResult;
using nevr_host::RuntimeStopText;
using nevr_host::StopLineFacts;
using nevr_test::StderrCapture;

constexpr std::uint32_t kImageGuardExitCode = 0xE0420003u;

std::wstring MakeTempDir(const wchar_t* tag) {
  wchar_t base[MAX_PATH] = {};
  const DWORD base_len = GetTempPathW(MAX_PATH, base);
  EXPECT_GT(base_len, 0u);
  static LONG counter = 0;
  const LONG sequence = InterlockedIncrement(&counter);
  wchar_t dir[MAX_PATH] = {};
  const int n = swprintf(dir, MAX_PATH, L"%ls\\nevr-stop-%ls-%lu-%llu-%ld", base, tag,
                         static_cast<unsigned long>(GetCurrentProcessId()),
                         static_cast<unsigned long long>(GetTickCount64()), static_cast<long>(sequence));
  EXPECT_GT(n, 0);
  EXPECT_NE(CreateDirectoryW(dir, nullptr), FALSE);
  return std::wstring(dir);
}

void MakeFile(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
}

std::string ReadAll(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string out;
  char buffer[512] = {};
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) out.append(buffer, read);
  CloseHandle(file);
  return out;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    EXPECT_NE(end, std::string::npos) << "a line without its newline";
    if (end == std::string::npos) break;
    lines.push_back(text.substr(0, end).substr(start));
    start = end + 1;
  }
  return lines;
}

std::string Utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
  return out;
}

// The keys of a record in the order the text has them (a parsed object sorts them).
std::vector<std::string> KeyOrder(const std::string& line) {
  std::vector<std::string> keys;
  const nlohmann::json parsed = nlohmann::json::parse(line);
  std::vector<std::pair<std::size_t, std::string>> at;
  for (auto it = parsed.begin(); it != parsed.end(); ++it) {
    const std::size_t position = line.find("\"" + it.key() + "\":");
    EXPECT_NE(position, std::string::npos) << it.key();
    at.emplace_back(position, it.key());
  }
  std::sort(at.begin(), at.end());
  for (const auto& entry : at) keys.push_back(entry.second);
  return keys;
}

std::string Build(const StopLineFacts& facts) {
  char buffer[nevr_host::kStopLineBytes] = {};
  const std::size_t length = nevr_host::BuildStopLine(buffer, sizeof(buffer), facts);
  return std::string(buffer, length);
}

nlohmann::json ParseOne(const std::string& text) {
  EXPECT_FALSE(text.empty());
  const std::vector<std::string> lines = Lines(text);
  EXPECT_EQ(lines.size(), 1u) << text;
  return lines.empty() ? nlohmann::json() : nlohmann::json::parse(lines[0]);
}

RuntimeLoadResult Result(RuntimeLoadOutcome outcome, const wchar_t* path) {
  RuntimeLoadResult result;
  result.outcome = outcome;
  result.path_ok = path != nullptr;
  if (path != nullptr) wcscpy(result.path, path);
  return result;
}

std::string DetailOf(const BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result) {
  RuntimeStopText text;
  nevr_host::DescribeRuntimeStop(diagnostics, result, text);
  return text.facts.detail == nullptr ? std::string() : std::string(text.facts.detail);
}

}  // namespace

TEST(StopLine, CarriesTheFieldsInTheDocumentedOrderWithTheTagAndEvent) {
  StopLineFacts facts;
  facts.stopped = true;
  facts.code = 78;
  facts.reason = "runtime_absent";
  facts.host_version = "5.8.0";
  facts.runtime_version = "5.8.1";
  facts.detail = "nevr-runtime.dll is missing";
  const wchar_t log[] = L"C:\\logs\\nevr-bootstrap.jsonl";
  facts.log = log;

  const std::string text = Build(facts);
  const std::vector<std::string> expected = {"tag", "ts", "pid", "event", "stopped", "code", "reason",
                                             "host_version", "runtime_version", "detail", "log"};
  const std::vector<std::string> lines = Lines(text);
  ASSERT_EQ(lines.size(), 1u) << text;
  EXPECT_EQ(KeyOrder(lines[0]), expected) << text;
  const nlohmann::json record = nlohmann::json::parse(lines[0]);
  EXPECT_EQ(record.at("tag"), "NEVR.STOP");
  EXPECT_EQ(record.at("event"), "stop");
  EXPECT_EQ(record.at("pid"), GetCurrentProcessId());
  EXPECT_EQ(record.at("stopped"), true);
  EXPECT_TRUE(record.at("code").is_number_unsigned());
  EXPECT_EQ(record.at("code"), 78);
  EXPECT_EQ(record.at("reason"), "runtime_absent");
  EXPECT_EQ(record.at("host_version"), "5.8.0");
  EXPECT_EQ(record.at("runtime_version"), "5.8.1");
  EXPECT_EQ(record.at("detail"), "nevr-runtime.dll is missing");
  EXPECT_EQ(record.at("log"), "C:\\logs\\nevr-bootstrap.jsonl");
}

TEST(StopLine, TheImageGuardCodeIsANumberNotAString) {
  StopLineFacts facts;
  facts.code = kImageGuardExitCode;
  facts.reason = "unsupported_image";
  facts.host_version = "5.8.0";
  const nlohmann::json record = ParseOne(Build(facts));
  EXPECT_TRUE(record.at("code").is_number_unsigned());
  EXPECT_EQ(record.at("code").get<std::uint64_t>(), 3762421763ull);
}

TEST(StopLine, ALineThatLeavesTheGameRunningCarriesNoCode) {
  StopLineFacts facts;
  facts.stopped = false;
  facts.code = 78;  // ignored when stopped is false
  facts.reason = "host_not_attached";
  facts.host_version = "5.8.0";
  const std::string text = Build(facts);
  const nlohmann::json record = ParseOne(text);
  EXPECT_EQ(record.at("stopped"), false);
  EXPECT_FALSE(record.contains("code")) << text;
  EXPECT_EQ(record.at("reason"), "host_not_attached");
}

TEST(StopLine, TheRuntimeVersionIsNotLoadedWhenTheHostDidNotReadTheFile) {
  StopLineFacts facts;
  facts.host_version = "5.8.0";
  facts.runtime_version = nullptr;
  EXPECT_EQ(ParseOne(Build(facts)).at("runtime_version"), "not loaded");
  facts.runtime_version = "";
  EXPECT_EQ(ParseOne(Build(facts)).at("runtime_version"), "not loaded");
  facts.runtime_version = "5.8.1";
  EXPECT_EQ(ParseOne(Build(facts)).at("runtime_version"), "5.8.1");
}

TEST(StopLine, TheLogPathIsUtf8WithBackslashesEscapedAndEmptyWhenThereIsNone) {
  StopLineFacts facts;
  facts.host_version = "5.8.0";
  // C:\u<e acute>\nevr-bootstrap.jsonl
  const wchar_t path[] = {L'C', L':', L'\\', L'u', 0x00E9, L'\\', L'n', L'e', L'v', L'r', 0};
  facts.log = path;
  const std::string text = Build(facts);
  EXPECT_NE(text.find("C:\\\\u\xC3\xA9\\\\nevr"), std::string::npos) << text;
  EXPECT_EQ(ParseOne(text).at("log"), "C:\\u\xC3\xA9\\nevr");
  facts.log = nullptr;
  EXPECT_EQ(ParseOne(Build(facts)).at("log"), "");
}

TEST(StopLine, ADetailWithQuotesAndNewlinesStaysOneParseableLine) {
  StopLineFacts facts;
  facts.host_version = "5.8.0";
  const std::string tricky = "quote\" back\\slash nl\n tab\t end";
  facts.detail = tricky.c_str();
  EXPECT_EQ(ParseOne(Build(facts)).at("detail"), tricky);
}

TEST(StopLine, ADetailThatDoesNotFitIsNeverTruncatedIntoBrokenJson) {
  StopLineFacts facts;
  facts.host_version = "5.8.0";
  const std::string huge(nevr_host::kStopLineBytes * 2, 'x');
  facts.detail = huge.c_str();
  const nlohmann::json record = ParseOne(Build(facts));  // still one valid line
  EXPECT_EQ(record.at("tag"), "NEVR.STOP");
  EXPECT_EQ(record.at("error"), "record_too_large");
  EXPECT_FALSE(record.contains("detail"));
}

TEST(StopLine, WriteStopLineWritesTheLineToTheStandardErrorHandleOnce) {
  StderrCapture capture;
  StopLineFacts facts;
  facts.code = 78;
  facts.reason = "runtime_absent";
  facts.host_version = "5.8.0";
  ASSERT_TRUE(nevr_host::WriteStopLine(facts));
  const nlohmann::json record = ParseOne(capture.Text());
  EXPECT_EQ(record.at("tag"), "NEVR.STOP");
  EXPECT_EQ(record.at("reason"), "runtime_absent");
}

TEST(StopLine, WriteStopLineReportsFalseWhenThereIsNoStandardErrorHandle) {
  const HANDLE previous = GetStdHandle(STD_ERROR_HANDLE);
  SetStdHandle(STD_ERROR_HANDLE, nullptr);
  StopLineFacts facts;
  facts.host_version = "5.8.0";
  EXPECT_FALSE(nevr_host::WriteStopLine(facts));
  SetStdHandle(STD_ERROR_HANDLE, previous);
}

// The reason token of each outcome that stops the game, and the exit code every one of them carries.
TEST(StopLineRuntime, EveryOutcomeThatStopsTheGameHasItsOwnReasonAndTheSameExitCode) {
  struct Row {
    RuntimeLoadOutcome outcome;
    const char* reason;
  };
  const Row rows[] = {
      {RuntimeLoadOutcome::kNoPath, "no_path"},
      {RuntimeLoadOutcome::kRuntimeAbsent, "runtime_absent"},
      {RuntimeLoadOutcome::kDependencyLoadFailed, "dependency_load_failed"},
      {RuntimeLoadOutcome::kLoadFailed, "load_failed"},
      {RuntimeLoadOutcome::kMissingEntry, "missing_entry"},
      {RuntimeLoadOutcome::kNullDescriptor, "null_descriptor"},
      {RuntimeLoadOutcome::kAbiRejected, "abi_rejected"},
      {RuntimeLoadOutcome::kInitFailed, "init_failed"},
      {RuntimeLoadOutcome::kRuntimeRefused, "runtime_refused"},
  };
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
  for (const Row& row : rows) {
    RuntimeLoadResult result = Result(row.outcome, L"C:\\nevr\\nevr-runtime.dll");
    RuntimeStopText text;
    nevr_host::DescribeRuntimeStop(diagnostics, result, text);
    EXPECT_STREQ(text.facts.reason, row.reason);
    EXPECT_TRUE(text.facts.stopped) << row.reason;
    EXPECT_EQ(text.facts.code, 78u) << row.reason;
    EXPECT_STREQ(text.facts.host_version, nevr_host::Version()) << row.reason;
    EXPECT_FALSE(std::string(text.facts.detail).empty()) << row.reason;
  }
}

TEST(StopLineRuntime, AnAbsentRuntimeSaysItIsMissingAndWhereTheHostLooked) {
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
  const RuntimeLoadResult result = Result(RuntimeLoadOutcome::kRuntimeAbsent, L"C:\\nevr\\echovr\\nevr-runtime.dll");
  EXPECT_EQ(DetailOf(diagnostics, result), "nevr-runtime.dll is missing; path=C:\\nevr\\echovr\\nevr-runtime.dll");
}

TEST(StopLineRuntime, ARefusedRuntimeCarriesTheVerifiersOwnWordsAndNoVersion) {
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
  RuntimeLoadResult result = Result(RuntimeLoadOutcome::kRuntimeRefused, L"C:\\nevr\\echovr\\nevr-runtime.dll");
  result.verify_reason = "unsigned";
  RuntimeStopText text;
  nevr_host::DescribeRuntimeStop(diagnostics, result, text);
  EXPECT_EQ(std::string(text.facts.detail), "unsigned; path=C:\\nevr\\echovr\\nevr-runtime.dll");
  EXPECT_TRUE(text.facts.runtime_version == nullptr || text.facts.runtime_version[0] == '\0');
}

TEST(StopLineRuntime, ARuntimeTheHostReadStatesItsVersion) {
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
  RuntimeLoadResult result = Result(RuntimeLoadOutcome::kInitFailed, L"C:\\nevr\\echovr\\nevr-runtime.dll");
  result.runtime_version = "5.8.1";
  result.init_code = 3;
  RuntimeStopText text;
  nevr_host::DescribeRuntimeStop(diagnostics, result, text);
  EXPECT_STREQ(text.facts.runtime_version, "5.8.1");
  const std::string detail = text.facts.detail;
  EXPECT_NE(detail.find("initialize returned 3"), std::string::npos) << detail;
  EXPECT_NE(detail.find("path=C:\\nevr\\echovr\\nevr-runtime.dll"), std::string::npos) << detail;
}

TEST(StopLineRuntime, AnAbiRejectionNamesTheStatusAndBothVersions) {
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
  RuntimeLoadResult result = Result(RuntimeLoadOutcome::kAbiRejected, L"C:\\nevr\\nevr-runtime.dll");
  result.abi_status = nevr_runtime_abi::Status::kMajorMismatch;
  result.expected_major = 1;
  result.expected_minor = 0;
  result.actual_major = 2;
  result.actual_minor = 1;
  const std::string detail = DetailOf(diagnostics, result);
  EXPECT_NE(detail.find(nevr_runtime_abi::StatusName(nevr_runtime_abi::Status::kMajorMismatch)), std::string::npos) << detail;
  EXPECT_NE(detail.find("host abi 1.0"), std::string::npos) << detail;
  EXPECT_NE(detail.find("runtime abi 2.1"), std::string::npos) << detail;
}

TEST(StopLineRuntime, ALoadFailureNamesTheWin32Error) {
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
  RuntimeLoadResult result = Result(RuntimeLoadOutcome::kDependencyLoadFailed, L"C:\\nevr\\nevr-runtime.dll");
  result.win32_error = 126;
  const std::string detail = DetailOf(diagnostics, result);
  EXPECT_NE(detail.find("win32_error=126"), std::string::npos) << detail;
}

TEST(StopLineRuntime, ALegacyRuntimeBesideTheHostIsNamedWhenTheRuntimeIsAbsentFromItsRoot) {
  const std::wstring host_dir = MakeTempDir(L"host") + L"\\";
  const std::wstring root = MakeTempDir(L"root");
  MakeFile(host_dir + L"nevr-runtime.dll");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, host_dir.c_str());
  const RuntimeLoadResult result = Result(RuntimeLoadOutcome::kRuntimeAbsent, (root + L"\\nevr-runtime.dll").c_str());

  const std::string detail = DetailOf(diagnostics, result);
  const std::string host_without_slash = Utf8(host_dir.substr(0, host_dir.size() - 1));
  // The detail is the plain text (not yet JSON): backslashes are single.
  EXPECT_NE(detail.find("a nevr-runtime.dll is beside BugSplat64.dll in " + host_without_slash), std::string::npos) << detail;
  EXPECT_NE(detail.find("does not load"), std::string::npos) << detail;
  EXPECT_NE(detail.find("move it to " + Utf8(root)), std::string::npos) << detail;
  EXPECT_EQ(detail.find("nevr-runtime.dll is missing; path=" + Utf8(root) + "\\nevr-runtime.dll"), 0u) << detail;

  DeleteFileW((host_dir + L"nevr-runtime.dll").c_str());
  RemoveDirectoryW(host_dir.c_str());
  RemoveDirectoryW(root.c_str());
}

TEST(StopLineRuntime, NoLegacySentenceWhenNothingIsBesideTheHost) {
  const std::wstring host_dir = MakeTempDir(L"host") + L"\\";
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, host_dir.c_str());
  const RuntimeLoadResult result = Result(RuntimeLoadOutcome::kRuntimeAbsent, L"C:\\nevr\\nevr-runtime.dll");
  EXPECT_EQ(DetailOf(diagnostics, result).find("beside"), std::string::npos);
  RemoveDirectoryW(host_dir.c_str());
}

TEST(StopLineRuntime, ALegacyFileIsNotMentionedForAnOutcomeThatIsNotAbsent) {
  const std::wstring host_dir = MakeTempDir(L"host") + L"\\";
  MakeFile(host_dir + L"nevr-runtime.dll");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, host_dir.c_str());
  RuntimeLoadResult refused = Result(RuntimeLoadOutcome::kRuntimeRefused, L"C:\\nevr\\nevr-runtime.dll");
  refused.verify_reason = "unsigned";
  EXPECT_EQ(DetailOf(diagnostics, refused).find("beside"), std::string::npos);
  EXPECT_EQ(DetailOf(diagnostics, Result(RuntimeLoadOutcome::kLoadFailed, L"C:\\nevr\\nevr-runtime.dll")).find("beside"),
            std::string::npos);
  DeleteFileW((host_dir + L"nevr-runtime.dll").c_str());
  RemoveDirectoryW(host_dir.c_str());
}

TEST(StopLineRuntime, TheLogFieldIsThePathOfTheLogTheHostWrites) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  RuntimeLoadResult result = Result(RuntimeLoadOutcome::kRuntimeAbsent, L"C:\\nevr\\nevr-runtime.dll");
  RuntimeStopText text;
  nevr_host::DescribeRuntimeStop(diagnostics, result, text);
  EXPECT_EQ(std::wstring(text.facts.log), local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
}

TEST(StopLineRuntime, WriteRuntimeStopLineWritesOneLineWithTheDescribedFacts) {
  StderrCapture capture;
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
  const RuntimeLoadResult result = Result(RuntimeLoadOutcome::kRuntimeAbsent, L"C:\\nevr\\nevr-runtime.dll");
  ASSERT_TRUE(nevr_host::WriteRuntimeStopLine(diagnostics, result));
  const nlohmann::json record = ParseOne(capture.Text());
  EXPECT_EQ(record.at("tag"), "NEVR.STOP");
  EXPECT_EQ(record.at("stopped"), true);
  EXPECT_EQ(record.at("code"), 78);
  EXPECT_EQ(record.at("reason"), "runtime_absent");
  EXPECT_EQ(record.at("host_version"), nevr_host::Version());
  EXPECT_EQ(record.at("runtime_version"), "not loaded");
}

TEST(StopLineAttach, EachFailedStepHasItsOwnDetailThatSaysTheGameRunsWithoutTheRuntime) {
  EXPECT_STREQ(nevr_host::AttachFailureDetail(AttachFailure::kNone), "");
  const std::string image = nevr_host::AttachFailureDetail(AttachFailure::kImageCheck);
  const std::string prologue = nevr_host::AttachFailureDetail(AttachFailure::kPrologueCheck);
  const std::string hook = nevr_host::AttachFailureDetail(AttachFailure::kHookInstall);
  EXPECT_EQ(image.find("image_check rejected"), 0u) << image;
  EXPECT_EQ(prologue.find("prologue_check rejected"), 0u) << prologue;
  EXPECT_EQ(hook.find("hook_install failed"), 0u) << hook;
  for (const std::string& detail : {image, prologue, hook}) {
    EXPECT_NE(detail.find("the game runs without the runtime"), std::string::npos) << detail;
  }
}

TEST(StopLineAttach, TheFirstFailedTargetStepIsRemembered) {
  BootstrapDiagnostics diagnostics;
  EXPECT_EQ(diagnostics.FirstAttachFailure(), AttachFailure::kNone);
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kAccepted);
  EXPECT_EQ(diagnostics.FirstAttachFailure(), AttachFailure::kNone);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, 7, nullptr, HookStage::kCreate);
  EXPECT_EQ(diagnostics.FirstAttachFailure(), AttachFailure::kHookInstall);
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kRejected);
  EXPECT_EQ(diagnostics.FirstAttachFailure(), AttachFailure::kHookInstall) << "the first failure stays";
}

TEST(StopLineAttach, ARejectedImageAndAMismatchedPrologueAreFailuresOfTheirOwnStep) {
  BootstrapDiagnostics image;
  image.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kRejected);
  EXPECT_EQ(image.FirstAttachFailure(), AttachFailure::kImageCheck);
  BootstrapDiagnostics prologue;
  prologue.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  EXPECT_EQ(prologue.FirstAttachFailure(), AttachFailure::kPrologueCheck);
}

TEST(StopLineAttach, StepsThatAreNotTheAttachTargetsAreNotFailuresOfIt) {
  BootstrapDiagnostics diagnostics;
  diagnostics.RecordAttach(AttachStep::kInstallWait, AttachOutcome::kFailed);
  diagnostics.RecordAttach(AttachStep::kLogMutex, AttachOutcome::kFailed);
  diagnostics.RecordAttach(AttachStep::kSetGameModule, AttachOutcome::kRejected);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kAccepted, 0, nullptr, HookStage::kEnable,
                           nevr_host::AttachNote::kHookBytesUnread);
  EXPECT_EQ(diagnostics.FirstAttachFailure(), AttachFailure::kNone);
}

TEST(StopLineAttach, ReportNotAttachedWritesTheRecordAndTheLineWithoutACode) {
  const std::wstring local = MakeTempDir(L"local");
  StderrCapture capture;
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  nevr_host::ReportNotAttached(diagnostics);

  const nlohmann::json line = ParseOne(capture.Text());
  EXPECT_EQ(line.at("tag"), "NEVR.STOP");
  EXPECT_EQ(line.at("stopped"), false);
  EXPECT_FALSE(line.contains("code"));
  EXPECT_EQ(line.at("reason"), "host_not_attached");
  EXPECT_EQ(line.at("host_version"), nevr_host::Version());
  EXPECT_EQ(line.at("runtime_version"), "not loaded");
  EXPECT_EQ(line.at("detail"), nevr_host::AttachFailureDetail(AttachFailure::kPrologueCheck));
  EXPECT_EQ(line.at("log"), Utf8(local) + "\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");

  const std::vector<std::string> records = Lines(ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl"));
  ASSERT_EQ(records.size(), 1u);
  const nlohmann::json record = nlohmann::json::parse(records[0]);
  EXPECT_EQ(record.at("event"), "host_not_attached");
  EXPECT_EQ(record.at("stopped"), false);
  EXPECT_EQ(record.at("reason"), "host_not_attached");
  EXPECT_EQ(record.at("detail"), line.at("detail"));
  EXPECT_FALSE(record.contains("exit_code"));
}

TEST(StopLineAttach, ReportNotAttachedWritesNothingWhenNoStepFailed) {
  const std::wstring local = MakeTempDir(L"local");
  StderrCapture capture;
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  nevr_host::ReportNotAttached(diagnostics);
  EXPECT_EQ(capture.Text(), "");
  EXPECT_EQ(ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl"), "");
}

TEST(StopLineAttach, ReportUnsupportedImageWritesTheStopRecordAndTheLineWithTheGuardsCode) {
  const std::wstring local = MakeTempDir(L"local");
  StderrCapture capture;
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  nevr_host::ReportUnsupportedImage(diagnostics, kImageGuardExitCode);

  const nlohmann::json line = ParseOne(capture.Text());
  EXPECT_EQ(line.at("tag"), "NEVR.STOP");
  EXPECT_EQ(line.at("stopped"), true);
  EXPECT_EQ(line.at("code").get<std::uint64_t>(), 3762421763ull);
  EXPECT_EQ(line.at("reason"), "unsupported_image");
  EXPECT_EQ(line.at("runtime_version"), "not loaded");
  EXPECT_EQ(line.at("host_version"), nevr_host::Version());
  EXPECT_NE(std::string(line.at("detail")).find("not the supported echovr.exe build"), std::string::npos);

  const std::vector<std::string> records = Lines(ReadAll(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl"));
  ASSERT_EQ(records.size(), 1u);
  const nlohmann::json record = nlohmann::json::parse(records[0]);
  EXPECT_EQ(record.at("event"), "game_stop");
  EXPECT_EQ(record.at("reason"), "unsupported_image");
  EXPECT_EQ(record.at("exit_code").get<std::uint64_t>(), 3762421763ull);
  EXPECT_EQ(record.at("stopped"), true);
  EXPECT_EQ(record.at("detail"), line.at("detail"));
}

TEST(StopLineLogPath, IsTheFirstReadyDirectoryBeforeAnythingIsWritten) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), temp.c_str(), nullptr);
  wchar_t path[MAX_PATH] = {};
  ASSERT_TRUE(diagnostics.LogFilePath(path, MAX_PATH));
  EXPECT_EQ(std::wstring(path), local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
}

TEST(StopLineLogPath, FollowsTheDirectoryTheLastRecordReached) {
  const std::wstring temp = MakeTempDir(L"temp");
  // A local-app-data that is a file, so the primary directory cannot be created and the record falls back to %TEMP%.
  const std::wstring blocker_dir = MakeTempDir(L"blocker");
  MakeFile(blocker_dir + L"\\file");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest((blocker_dir + L"\\file").c_str(), temp.c_str(), nullptr);
  ASSERT_EQ(diagnostics.Emit(nlohmann::json{{"event", "probe"}}), nevr_host::DiagnosticDestination::kFallback);
  wchar_t path[MAX_PATH] = {};
  ASSERT_TRUE(diagnostics.LogFilePath(path, MAX_PATH));
  EXPECT_EQ(std::wstring(path), temp + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
}

TEST(StopLineLogPath, IsFalseWhenNoDirectoryIsReadyOrThePathDoesNotFit) {
  BootstrapDiagnostics none;
  none.PrepareForTest(nullptr, nullptr, nullptr);
  wchar_t path[MAX_PATH] = {L'x', 0};
  EXPECT_FALSE(none.LogFilePath(path, MAX_PATH));
  EXPECT_EQ(path[0], L'\0') << "out is emptied";

  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics some;
  some.PrepareForTest(local.c_str(), nullptr, nullptr);
  wchar_t small[8] = {L'x', 0};
  EXPECT_FALSE(some.LogFilePath(small, 8));
  EXPECT_EQ(small[0], L'\0');
}

TEST(StopLineLogPath, IsFalseAfterARecordThatReachedNoFile) {
  // Every destination refuses: the line went to stderr only, so no file holds it.
  StderrCapture capture;
  const std::wstring blocker_dir = MakeTempDir(L"blocker");
  MakeFile(blocker_dir + L"\\file");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest((blocker_dir + L"\\file").c_str(), nullptr, nullptr);
  ASSERT_EQ(diagnostics.Emit(nlohmann::json{{"event", "probe"}}), nevr_host::DiagnosticDestination::kDropped);
  wchar_t path[MAX_PATH] = {};
  EXPECT_FALSE(diagnostics.LogFilePath(path, MAX_PATH));
}

TEST(RuntimeFileBeside, FindsAFileNamedNevrRuntimeDllInTheDirectory) {
  const std::wstring dir = MakeTempDir(L"beside");
  EXPECT_FALSE(nevr_host::RuntimeFileBeside(dir.c_str()));
  MakeFile(dir + L"\\nevr-runtime.dll");
  EXPECT_TRUE(nevr_host::RuntimeFileBeside(dir.c_str()));
  EXPECT_TRUE(nevr_host::RuntimeFileBeside((dir + L"\\").c_str())) << "a trailing backslash is accepted";
  EXPECT_FALSE(nevr_host::RuntimeFileBeside(nullptr));
  EXPECT_FALSE(nevr_host::RuntimeFileBeside(L""));
  EXPECT_FALSE(nevr_host::RuntimeFileBeside((dir + L"\\missing").c_str()));
  DeleteFileW((dir + L"\\nevr-runtime.dll").c_str());
  RemoveDirectoryW(dir.c_str());
}

TEST(RuntimeFileBeside, ADirectoryOfThatNameIsNotARuntime) {
  const std::wstring dir = MakeTempDir(L"beside");
  ASSERT_NE(CreateDirectoryW((dir + L"\\nevr-runtime.dll").c_str(), nullptr), FALSE);
  EXPECT_FALSE(nevr_host::RuntimeFileBeside(dir.c_str()));
  RemoveDirectoryW((dir + L"\\nevr-runtime.dll").c_str());
  RemoveDirectoryW(dir.c_str());
}

// The words in the dialog a client sees: the sentence for the cause, then which build and which log, so a screenshot
// is enough for support.
TEST(StopNoticeBody, AddsTheVersionAndTheLogPathAfterABlankLine) {
  const std::wstring body = nevr_host::StopNoticeBody(L"nEVR couldn't start: x.", "5.8.0", L"C:\\logs\\nevr-bootstrap.jsonl");
  EXPECT_EQ(body, L"nEVR couldn't start: x.\n\nnEVR 5.8.0\nLog: C:\\logs\\nevr-bootstrap.jsonl");
}

TEST(StopNoticeBody, LeavesTheLogLineOutWhenNoFileTookTheRecord) {
  EXPECT_EQ(nevr_host::StopNoticeBody(L"nEVR couldn't start: x.", "5.8.0", nullptr), L"nEVR couldn't start: x.\n\nnEVR 5.8.0");
  EXPECT_EQ(nevr_host::StopNoticeBody(L"nEVR couldn't start: x.", "5.8.0", L""), L"nEVR couldn't start: x.\n\nnEVR 5.8.0");
}
