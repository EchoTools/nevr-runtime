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

constexpr char kDetailIncomplete[] = " [detail incomplete: a path did not fit]";

const char* OrEmpty(const char* text) noexcept { return text == nullptr ? "" : text; }

// A detail under construction in a caller's fixed buffer: no heap. A piece that does not fit is left out whole, so the
// text never ends in the middle of a UTF-8 sequence, and the detail then ends with kDetailIncomplete (Finish), so a
// reader is told that something is missing rather than reading a sentence that stops early.
class Text {
 public:
  Text(char* out, std::size_t size) noexcept
      : out_(out), size_(size > sizeof(kDetailIncomplete) ? size - sizeof(kDetailIncomplete) : 0), room_(size) {
    if (room_ != 0) out_[0] = '\0';
  }

  // Ends the detail: when a piece was left out, the marker takes the room kept for it.
  void Finish() noexcept {
    if (truncated_ && room_ >= used_ + sizeof(kDetailIncomplete)) {
      std::memcpy(out_ + used_, kDetailIncomplete, sizeof(kDetailIncomplete));
      used_ += sizeof(kDetailIncomplete) - 1;
    }
  }

  std::size_t Used() const noexcept { return used_; }

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
  std::size_t size_;   // room for pieces; the marker's room is kept beyond it
  std::size_t room_;   // the whole buffer
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

// Writes all `length` bytes to `handle`, continuing a short write. False when there is no handle or a write fails.
bool WriteAll(HANDLE handle, const char* text, std::size_t length) noexcept {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
  std::size_t written = 0;
  while (written < length) {
    DWORD chunk = 0;
    if (WriteFile(handle, text + written, static_cast<DWORD>(length - written), &chunk, nullptr) == FALSE || chunk == 0) {
      return false;
    }
    written += chunk;
  }
  return true;
}

// The plain notice (#1533) goes to the first console that takes it: standard error; else the console of the parent
// process, which a GUI-subsystem game started from a shell has no handle to (AttachConsole fails when there is no
// parent console or this process already has one, and CONOUT$ then names the console there is); else standard output.
bool PutConsoleText(const char* text, std::size_t length) noexcept {
  if (WriteAll(GetStdHandle(STD_ERROR_HANDLE), text, length)) return true;
  static_cast<void>(AttachConsole(ATTACH_PARENT_PROCESS));
  const HANDLE console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (console != INVALID_HANDLE_VALUE) {
    const bool written = WriteAll(console, text, length);
    CloseHandle(console);
    if (written) return true;
  }
  return WriteAll(GetStdHandle(STD_OUTPUT_HANDLE), text, length);
}

// `sentence`, the detail and the log path as one line of text with its newline. 0 when `capacity` cannot hold it.
std::size_t BuildNoticeLine(char* buffer, std::size_t capacity, const wchar_t* sentence, const char* detail,
                            const wchar_t* log) noexcept {
  if (capacity < 2) return 0;
  Text text(buffer, capacity - 1);  // the last byte is the newline
  text.AddWide(sentence).Add(" Detail: ").Add(detail);
  if (log != nullptr && log[0] != L'\0') text.Add(" Log: ").AddWide(log);
  text.Finish();
  const std::size_t used = text.Used();
  buffer[used] = '\n';
  buffer[used + 1] = '\0';
  return used + 1;
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

bool PutStopLine(const StopLineFacts& facts) noexcept {
  char buffer[kStopLineBytes];
  const std::size_t length = BuildStopLine(buffer, sizeof(buffer), facts);
  if (length == 0) return false;
  // WriteAll continues a short write from where it stopped, so the line is never left without its tail.
  return WriteAll(GetStdHandle(STD_ERROR_HANDLE), buffer, length);
}

void DescribeRuntimeStop(const BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result,
                         RuntimeStopText& out) noexcept {
  Text text(out.detail, sizeof(out.detail));
  DescribeOutcome(result, text);
  DescribeLegacyRuntime(diagnostics, result, text);
  text.Finish();
  out.facts.stopped = true;
  out.facts.code = kRuntimeStopExitCode;
  out.facts.reason = result.OutcomeName();
  out.facts.host_version = Version();
  out.facts.runtime_version = result.runtime_version.c_str();
  out.facts.detail = out.detail;
  out.facts.log = diagnostics.LogFilePath(out.log, MAX_PATH) ? out.log : nullptr;
}

bool WriteRuntimeStopLine(const BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result) noexcept {
  RuntimeStopText text;
  DescribeRuntimeStop(diagnostics, result, text);
  return PutStopLine(text.facts);
}

bool WriteRuntimeNoticeLine(const wchar_t* sentence, BootstrapDiagnostics& diagnostics,
                            const RuntimeLoadResult& result) noexcept {
  RuntimeStopText text;
  DescribeRuntimeStop(diagnostics, result, text);
  char buffer[kNoticeLineBytes];
  const std::size_t length = BuildNoticeLine(buffer, sizeof(buffer), sentence, text.detail, text.facts.log);
  const bool written = length != 0 && PutConsoleText(buffer, length);
  // The host log says whether a console took the line, so a run with no console is visible in the record.
  try {
    nlohmann::json record;
    record["event"] = "console_notice";
    record["reason"] = result.OutcomeName();
    record["written"] = written;
    diagnostics.Emit(record);
  } catch (const std::exception& error) {
    char buffer_json[BootstrapDiagnostics::kMaxRecordBytes];
    JsonLine line(buffer_json, sizeof(buffer_json), "console_notice");
    line.Str("error", "record_build_failed").Str("what", error.what()).Bool("written", written);
    const std::size_t line_length = line.Finish();
    if (line_length != 0) diagnostics.AppendLine(buffer_json, line_length);
  }
  return written;
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
  PutStopLine(text.facts);
}

void ReportNotAttached(BootstrapDiagnostics& diagnostics) noexcept {
  const AttachFailure failure = diagnostics.FirstAttachFailure();
  if (failure == AttachFailure::kNone) return;
  RuntimeStopText text;
  FillHostFacts(diagnostics, false, kReasonHostNotAttached, AttachFailureDetail(failure), text);
  EmitFactsRecord(diagnostics, "host_not_attached", text.facts);
  text.facts.log = diagnostics.LogFilePath(text.log, MAX_PATH) ? text.log : nullptr;
  PutStopLine(text.facts);
}

}  // namespace nevr_host
