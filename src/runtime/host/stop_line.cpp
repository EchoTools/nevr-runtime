#include "runtime/host/stop_line.h"

#include "core/runtime_abi.h"
#include "runtime/host/host_identity.h"
#include "runtime/host/host_json.h"
#include "runtime/host/path_buffer.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>

namespace nevr_host {
namespace {

const char* OrEmpty(const char* text) noexcept { return text == nullptr ? "" : text; }

// A detail under construction in a caller's fixed buffer: no heap. A piece that does not fit is left out whole and
// remembered, so the text never ends in the middle of a UTF-8 sequence.
class Text {
 public:
  Text(char* out, std::size_t size) noexcept : out_(out), size_(size) {
    if (size_ != 0) out_[0] = '\0';
  }

  Text& Add(const char* piece) noexcept {
    const std::size_t length = std::strlen(piece);
    if (length + 1 > size_ - used_) {
      truncated_ = true;
      return *this;
    }
    std::memcpy(out_ + used_, piece, length + 1);
    used_ += length;
    return *this;
  }

  Text& AddWide(const wchar_t* piece) noexcept {
    const int needed = WideCharToMultiByte(CP_UTF8, 0, piece, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0 || static_cast<std::size_t>(needed) > size_ - used_) {
      truncated_ = true;
      return *this;
    }
    WideCharToMultiByte(CP_UTF8, 0, piece, -1, out_ + used_, needed, nullptr, nullptr);
    used_ += static_cast<std::size_t>(needed) - 1;
    return *this;
  }

  Text& AddUInt(std::uint64_t value) noexcept {
    char digits[24];
    std::size_t at = sizeof(digits) - 1;
    digits[at] = '\0';
    do {
      digits[--at] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value != 0);
    return Add(digits + at);
  }

  Text& AddInt(std::int64_t value) noexcept {
    if (value < 0) {
      Add("-");
      return AddUInt(static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(value));
    }
    return AddUInt(static_cast<std::uint64_t>(value));
  }

  Text& AddVersion(std::uint32_t major, std::uint32_t minor) noexcept {
    return AddUInt(major).Add(".").AddUInt(minor);
  }

 private:
  char* out_;
  std::size_t size_;
  std::size_t used_ = 0;
  bool truncated_ = false;
};

// The directory part of `path` without its trailing backslash, into `out` (empty when the path has none).
void DirectoryOf(const wchar_t* path, wchar_t* out, std::size_t size) noexcept {
  out[0] = L'\0';
  const wchar_t* separator = std::wcsrchr(path, L'\\');
  if (separator == nullptr) return;
  const std::size_t length = static_cast<std::size_t>(separator - path);
  if (length + 1 > size) return;
  std::memcpy(out, path, length * sizeof(wchar_t));
  out[length] = L'\0';
}

// `directory` without a trailing backslash, into `out`.
void WithoutTrailingBackslash(const wchar_t* directory, wchar_t* out, std::size_t size) noexcept {
  out[0] = L'\0';
  std::size_t length = std::wcslen(directory);
  while (length > 0 && directory[length - 1] == L'\\') --length;
  if (length + 1 > size) return;
  std::memcpy(out, directory, length * sizeof(wchar_t));
  out[length] = L'\0';
}

void DescribeOutcome(const RuntimeLoadResult& result, Text& text) noexcept {
  switch (result.outcome) {
    case RuntimeLoadOutcome::kLoaded:
      text.Add("the runtime loaded");
      return;
    case RuntimeLoadOutcome::kNoPath:
      text.Add("no nEVR root could be formed: %LOCALAPPDATA% is not set or the path does not fit");
      return;
    case RuntimeLoadOutcome::kRuntimeAbsent:
      text.Add("nevr-runtime.dll is missing");
      break;
    case RuntimeLoadOutcome::kRuntimeRefused:
      text.Add(result.verify_reason.empty() ? "refused" : result.verify_reason.c_str());
      break;
    case RuntimeLoadOutcome::kDependencyLoadFailed:
      text.Add("a DLL that nevr-runtime.dll imports could not be loaded; win32_error=").AddUInt(result.win32_error);
      break;
    case RuntimeLoadOutcome::kLoadFailed:
      text.Add("nevr-runtime.dll could not be loaded; win32_error=").AddUInt(result.win32_error);
      break;
    case RuntimeLoadOutcome::kMissingEntry:
      text.Add("nevr-runtime.dll has no ").Add(nevr_runtime_abi::kGetRuntimeApiExport).Add(" export");
      break;
    case RuntimeLoadOutcome::kNullDescriptor:
      text.Add(nevr_runtime_abi::kGetRuntimeApiExport).Add(" returned no descriptor");
      break;
    case RuntimeLoadOutcome::kAbiRejected:
      text.Add("ABI rejected: ").Add(nevr_runtime_abi::StatusName(result.abi_status)).Add("; host abi ");
      text.AddVersion(result.expected_major, result.expected_minor).Add(", runtime abi ");
      text.AddVersion(result.actual_major, result.actual_minor);
      break;
    case RuntimeLoadOutcome::kInitFailed:
      if (result.init_threw) {
        text.Add("initialize threw an exception");
      } else {
        text.Add("initialize returned ").AddInt(result.init_code);
      }
      break;
  }
  if (result.path_ok) text.Add("; path=").AddWide(result.path);
}

// The old layout: a nevr-runtime.dll left beside BugSplat64.dll, which the host no longer loads.
void DescribeLegacyRuntime(const BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result,
                           Text& text) noexcept {
  if (result.outcome != RuntimeLoadOutcome::kRuntimeAbsent || !result.path_ok || !diagnostics.HostDirReady()) return;
  if (!RuntimeFileBeside(diagnostics.HostDir())) return;
  wchar_t host_dir[MAX_PATH] = {};
  wchar_t root[MAX_PATH] = {};
  WithoutTrailingBackslash(diagnostics.HostDir(), host_dir, MAX_PATH);
  DirectoryOf(result.path, root, MAX_PATH);
  text.Add("; a nevr-runtime.dll is beside BugSplat64.dll in ").AddWide(host_dir);
  text.Add(", which the host does not load");
  if (root[0] != L'\0') text.Add(": move it to ").AddWide(root);
}

std::string ToUtf8(const wchar_t* text) {
  if (text == nullptr || text[0] == L'\0') return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
  out.resize(static_cast<std::size_t>(needed) - 1);
  return out;
}

// The facts of a stop or a failed attach that are not about the runtime's load: the host's version, no runtime version.
void FillHostFacts(const BootstrapDiagnostics& diagnostics, bool stopped, const char* reason, const char* detail,
                   RuntimeStopText& out) noexcept {
  std::strncpy(out.detail, detail, sizeof(out.detail) - 1);
  out.facts.stopped = stopped;
  out.facts.reason = reason;
  out.facts.host_version = Version();
  out.facts.runtime_version = nullptr;
  out.facts.detail = out.detail;
  out.facts.log = diagnostics.LogFilePath(out.log, MAX_PATH) ? out.log : nullptr;
}

// One record of the host log for a stop or a failed attach: `event`, the facts, and `exit_code` for a stop. Never throws;
// a record that cannot be built is a marker line, as every host record.
void EmitFactsRecord(BootstrapDiagnostics& diagnostics, const char* event, const StopLineFacts& facts) noexcept {
  try {
    nlohmann::json record;
    record["event"] = event;
    record["reason"] = facts.reason;
    if (facts.stopped) {
      record["exit_code"] = facts.code;
      record["host_identity"] = IdentityLiteral();
    }
    AddStopFacts(record, facts);
    diagnostics.Emit(record);
  } catch (const std::exception& error) {
    char buffer[BootstrapDiagnostics::kMaxRecordBytes];
    JsonLine line(buffer, sizeof(buffer), event);
    line.Str("error", "record_build_failed").Str("what", error.what()).Str("reason", facts.reason);
    const std::size_t length = line.Finish();
    if (length != 0) diagnostics.AppendLine(buffer, length);
  }
}

}  // namespace

std::size_t BuildStopLine(char* buffer, std::size_t capacity, const StopLineFacts& facts) noexcept {
  JsonLine line(buffer, capacity, kStopTag, "stop");
  line.Bool("stopped", facts.stopped);
  if (facts.stopped) line.UInt("code", facts.code);
  line.Str("reason", facts.reason).Str("host_version", facts.host_version);
  const bool runtime_known = facts.runtime_version != nullptr && facts.runtime_version[0] != '\0';
  line.Str("runtime_version", runtime_known ? facts.runtime_version : kRuntimeNotLoaded);
  line.Str("detail", facts.detail);
  if (facts.log != nullptr && facts.log[0] != L'\0') {
    line.Wide("log", facts.log);
  } else {
    line.Str("log", "");
  }
  return line.Finish();
}

bool WriteStopLine(const StopLineFacts& facts) noexcept {
  char buffer[kStopLineBytes];
  const std::size_t length = BuildStopLine(buffer, sizeof(buffer), facts);
  if (length == 0) return false;
  const HANDLE standard_error = GetStdHandle(STD_ERROR_HANDLE);
  if (standard_error == nullptr || standard_error == INVALID_HANDLE_VALUE) return false;
  // A short write is continued from where it stopped, so the line is never left without its tail.
  std::size_t written = 0;
  while (written < length) {
    DWORD chunk = 0;
    if (WriteFile(standard_error, buffer + written, static_cast<DWORD>(length - written), &chunk, nullptr) == FALSE ||
        chunk == 0) {
      return false;
    }
    written += chunk;
  }
  return true;
}

void DescribeRuntimeStop(const BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result,
                         RuntimeStopText& out) noexcept {
  Text text(out.detail, sizeof(out.detail));
  DescribeOutcome(result, text);
  DescribeLegacyRuntime(diagnostics, result, text);
  out.facts.stopped = true;
  out.facts.code = kRuntimeStopExitCode;
  out.facts.reason = result.OutcomeName();
  out.facts.host_version = Version();
  out.facts.runtime_version = result.runtime_version.c_str();
  out.facts.detail = out.detail;
  out.facts.log = diagnostics.LogFilePath(out.log, MAX_PATH) ? out.log : nullptr;
}

void AddStopFacts(nlohmann::json& record, const StopLineFacts& facts) {
  const bool runtime_known = facts.runtime_version != nullptr && facts.runtime_version[0] != '\0';
  record["stopped"] = facts.stopped;
  record["host_version"] = OrEmpty(facts.host_version);
  record["runtime_version"] = runtime_known ? facts.runtime_version : kRuntimeNotLoaded;
  record["detail"] = OrEmpty(facts.detail);
  record["log"] = ToUtf8(facts.log);
}

const char* AttachFailureDetail(AttachFailure failure) noexcept {
  switch (failure) {
    case AttachFailure::kNone:
      return "";
    case AttachFailure::kImageCheck:
      return "image_check rejected: the game image is not the supported echovr.exe build; the game runs without the "
             "runtime";
    case AttachFailure::kPrologueCheck:
      return "prologue_check rejected: GameMainWrapper does not start with the pinned bytes; the game runs without the "
             "runtime";
    case AttachFailure::kHookInstall:
      return "hook_install failed: the GameMainWrapper detour could not be installed; the game runs without the "
             "runtime";
  }
  return "";
}

void ReportUnsupportedImage(BootstrapDiagnostics& diagnostics, std::uint32_t exit_code) noexcept {
  RuntimeStopText text;
  FillHostFacts(diagnostics, true, kReasonUnsupportedImage,
                "the game image is not the supported echovr.exe build; image_check rejected", text);
  text.facts.code = exit_code;
  EmitFactsRecord(diagnostics, "game_stop", text.facts);
  // The log the record reached, now that it has been written.
  text.facts.log = diagnostics.LogFilePath(text.log, MAX_PATH) ? text.log : nullptr;
  WriteStopLine(text.facts);
}

void ReportNotAttached(BootstrapDiagnostics& diagnostics) noexcept {
  const AttachFailure failure = diagnostics.FirstAttachFailure();
  if (failure == AttachFailure::kNone) return;
  RuntimeStopText text;
  FillHostFacts(diagnostics, false, kReasonHostNotAttached, AttachFailureDetail(failure), text);
  EmitFactsRecord(diagnostics, "host_not_attached", text.facts);
  text.facts.log = diagnostics.LogFilePath(text.log, MAX_PATH) ? text.log : nullptr;
  WriteStopLine(text.facts);
}

}  // namespace nevr_host
