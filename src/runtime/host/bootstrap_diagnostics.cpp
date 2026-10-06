#include "runtime/host/bootstrap_diagnostics.h"

#include "runtime/host/path_buffer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <string>

namespace NevrHost {
namespace {

constexpr wchar_t kLogsSuffix[] = L"EchoVR\\logs";
constexpr wchar_t kFileName[] = L"nevr-bootstrap.jsonl";

}  // namespace

void BootstrapDiagnostics::Prepare() {
  primary_ready_ = false;
  fallback_ready_ = false;

  const bool local_ok = CopyEnv(local_app_data_, MAX_PATH, L"LOCALAPPDATA");
  const bool temp_ok = CopyEnv(temp_, MAX_PATH, L"TEMP");
  if (local_ok) primary_ready_ = AppendPath(primary_dir_, MAX_PATH, local_app_data_, kLogsSuffix);
  if (temp_ok) fallback_ready_ = AppendPath(fallback_dir_, MAX_PATH, temp_, kLogsSuffix);
}

void BootstrapDiagnostics::RecordAttachFailure(AttachFailure failure, std::uint32_t detail) {
  if (failure == AttachFailure::kNone) return;
  attach_failure_ = failure;
  attach_detail_ = detail;
}

void BootstrapDiagnostics::FlushPendingAttachDiagnostic() {
  if (attach_failure_ == AttachFailure::kNone) return;
  if (attach_flushed_.exchange(true)) return;  // one record per attach, ever
  nlohmann::json record;
  record["event"] = "host_attach";
  const char* outcome = "hook_failed";
  switch (attach_failure_) {
    case AttachFailure::kImageRejected: outcome = "image_rejected"; break;
    case AttachFailure::kPrologueMismatch: outcome = "prologue_mismatch"; break;
    case AttachFailure::kHookFailed: outcome = "hook_failed"; break;
    case AttachFailure::kNone: return;  // unreachable; guarded above
  }
  record["outcome"] = outcome;
  record["degraded_action"] = "run_original_only";
  if (attach_detail_ != 0) record["detail"] = attach_detail_;
  Emit(record);
}

DiagnosticDestination BootstrapDiagnostics::Emit(const nlohmann::json& record) {
  std::string line = record.dump();
  if (line.size() > kMaxRecordBytes) return DiagnosticDestination::kDropped;
  line.push_back('\n');

  if (primary_ready_ && EmitTo(primary_dir_, line.data(), line.size())) {
    return DiagnosticDestination::kPrimary;
  }
  if (fallback_ready_ && EmitTo(fallback_dir_, line.data(), line.size())) {
    return DiagnosticDestination::kFallback;
  }

  // Loud: a diagnostic that could not be persisted still reaches the console.
  HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
  if (stderr_handle != nullptr && stderr_handle != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(stderr_handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  }
  return DiagnosticDestination::kDropped;
}

bool BootstrapDiagnostics::EmitTo(const wchar_t* dir, const char* line, std::size_t length) {
  wchar_t path[MAX_PATH] = {};
  if (!AppendPath(path, MAX_PATH, dir, kFileName)) return false;

  // Create the directory (and intermediate levels) on demand.
  wchar_t dir_copy[MAX_PATH] = {};
  const std::size_t dir_len = wcslen(dir);
  if (dir_len + 1 > MAX_PATH) return false;
  std::memcpy(dir_copy, dir, (dir_len + 1) * sizeof(wchar_t));
  CreateDirectories(dir_copy);

  HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  DWORD written = 0;
  const BOOL ok = WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
  FlushFileBuffers(file);
  CloseHandle(file);
  return ok != FALSE && written == length;
}

}  // namespace NevrHost
