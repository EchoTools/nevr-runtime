#include "runtime/host/bootstrap_diagnostics.h"

#include "runtime/host/host_json.h"
#include "runtime/host/host_paths.h"
#include "runtime/host/path_buffer.h"
#include "runtime/host/game_targets.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace nevr_host {
namespace {

const char* StepName(AttachStep step) noexcept {
  switch (step) {
    case AttachStep::kImageCheck: return "image_check";
    case AttachStep::kPrologueCheck: return "prologue_check";
    case AttachStep::kHookInstall: return "hook_install";
    case AttachStep::kInstallWait: return "install_wait";
    case AttachStep::kDetourEntry: return "detour_entry";
    case AttachStep::kSetGameModule: return "set_game_module";
    case AttachStep::kJournal: return "journal";
  }
  return "unknown";
}

// The outcome token depends on the step: "accepted" for an image or prologue
// check reads as "enabled" for the hook, and a rejected prologue is a "mismatch".
const char* OutcomeName(AttachStep step, AttachOutcome outcome, AttachNote note) noexcept {
  switch (step) {
    case AttachStep::kInstallWait:
      if (outcome == AttachOutcome::kAccepted) return "signalled";
      return note == AttachNote::kInstallWaitTimeout ? "timeout" : "unavailable";
    case AttachStep::kDetourEntry: return "defect";
    case AttachStep::kSetGameModule: return "refused";
    case AttachStep::kJournal: return "overflow";
    case AttachStep::kImageCheck:
    case AttachStep::kPrologueCheck:
    case AttachStep::kHookInstall:
      break;
  }
  switch (outcome) {
    case AttachOutcome::kAccepted:
      return step == AttachStep::kHookInstall ? "enabled" : "accepted";
    case AttachOutcome::kRejected:
      return step == AttachStep::kPrologueCheck ? "mismatch" : "rejected";
    case AttachOutcome::kFailed:
      return "failed";
  }
  return "unknown";
}

const char* StageName(HookStage stage) noexcept {
  switch (stage) {
    case HookStage::kNone: return "none";
    case HookStage::kInitialize: return "initialize";
    case HookStage::kCreate: return "create";
    case HookStage::kEnable: return "enable";
  }
  return "unknown";
}

std::string HexBytes(const unsigned char* bytes, std::size_t count) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(count * 2);
  for (std::size_t at = 0; at < count; ++at) {
    out.push_back(kDigits[bytes[at] >> 4]);
    out.push_back(kDigits[bytes[at] & 0xF]);
  }
  return out;
}

// A record that could not be built (allocation failure) is still reported: on stderr,
// in a fixed buffer, with the exception text.
void ReportBuildFailure(const char* event, const char* what) noexcept {
  HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
  if (stderr_handle == nullptr || stderr_handle == INVALID_HANDLE_VALUE) return;
  char buffer[512];
  JsonLine line(buffer, sizeof(buffer), event);
  line.Str("error", "record_build_failed").Str("what", what);
  const std::size_t length = line.Finish();
  DWORD written = 0;
  if (length != 0) WriteFile(stderr_handle, buffer, static_cast<DWORD>(length), &written, nullptr);
}

// The log's mutex, created on first use and kept for the process: no allocation, so it is safe in
// the crash callback. Null when it cannot be created (appends then run unserialized).
HANDLE LogMutex() noexcept {
  static std::atomic<HANDLE> mutex{nullptr};
  HANDLE existing = mutex.load(std::memory_order_acquire);
  if (existing != nullptr) return existing;
  HANDLE created = CreateMutexW(nullptr, FALSE, kBootstrapLogMutexName);
  if (created == nullptr) return nullptr;
  if (mutex.compare_exchange_strong(existing, created)) return created;
  CloseHandle(created);
  return existing;
}

}  // namespace

const char* DestinationName(DiagnosticDestination destination) noexcept {
  switch (destination) {
    case DiagnosticDestination::kPrimary: return "localappdata";
    case DiagnosticDestination::kFallback: return "temp";
    case DiagnosticDestination::kHostDirectory: return "host_directory";
    case DiagnosticDestination::kDropped: return "dropped";
  }
  return "unknown";
}

void BootstrapDiagnostics::Prepare(HMODULE host_module) {
  primary_ready_ = false;
  fallback_ready_ = false;
  host_dir_ready_ = false;

  const bool local_ok = CopyEnv(local_app_data_, MAX_PATH, L"LOCALAPPDATA");
  const bool temp_ok = CopyEnv(temp_, MAX_PATH, L"TEMP");
  if (local_ok) primary_ready_ = AppendPath(primary_dir_, MAX_PATH, local_app_data_, kLogsSubdirectory);
  if (temp_ok) fallback_ready_ = AppendPath(fallback_dir_, MAX_PATH, temp_, kLogsSubdirectory);
  host_dir_ready_ = DirOfModule(host_dir_, MAX_PATH, host_module);
}

void BootstrapDiagnostics::PrepareForTest(const wchar_t* local_app_data, const wchar_t* temp,
                                          const wchar_t* host_dir) {
  primary_ready_ = false;
  fallback_ready_ = false;
  host_dir_ready_ = false;
  if (local_app_data != nullptr && local_app_data[0] != L'\0') {
    primary_ready_ = AppendPath(primary_dir_, MAX_PATH, local_app_data, kLogsSubdirectory);
  }
  if (temp != nullptr && temp[0] != L'\0') {
    fallback_ready_ = AppendPath(fallback_dir_, MAX_PATH, temp, kLogsSubdirectory);
  }
  if (host_dir != nullptr && host_dir[0] != L'\0') {
    // The host directory is used as given: it already exists and is not created.
    const std::size_t length = wcslen(host_dir);
    if (length + 1 <= MAX_PATH) {
      std::memcpy(host_dir_, host_dir, (length + 1) * sizeof(wchar_t));
      host_dir_ready_ = true;
    }
  }
}

void BootstrapDiagnostics::RecordAttach(AttachStep step, AttachOutcome outcome,
                                        std::uint32_t detail, const unsigned char* actual,
                                        HookStage stage, AttachNote note) noexcept {
  const std::size_t slot = journal_count_.fetch_add(1);
  if (slot >= kJournalCapacity) {
    journal_count_.store(kJournalCapacity);  // saturate: the first entries are the ones kept
    dropped_.fetch_add(1);                   // and the rest are counted, then reported at the flush
    return;
  }
  JournalEntry& entry = journal_[slot];
  entry.step = step;
  entry.outcome = outcome;
  entry.detail = detail;
  entry.stage = stage;
  entry.note = note;
  entry.has_actual = actual != nullptr;
  if (actual != nullptr) std::memcpy(entry.actual, actual, kGameMainWrapperPrologueSize);
  entry.ready.store(1, std::memory_order_release);
}

bool BootstrapDiagnostics::AttachDegraded() const noexcept {
  const std::size_t count = journal_count_.load();
  for (std::size_t at = 0; at < count && at < kJournalCapacity; ++at) {
    if (journal_[at].ready.load(std::memory_order_acquire) == 0) continue;
    if (journal_[at].outcome != AttachOutcome::kAccepted) return true;
    if (journal_[at].note == AttachNote::kHookBytesUnread) return true;
  }
  return dropped_.load() != 0;
}

void BootstrapDiagnostics::FlushAttachJournal() noexcept {
  // Claim the entries from the written mark up to the first one that is not complete yet. RecordAttach
  // reserves a slot before it fills it, so the count can run ahead of what is written: an entry is
  // emitted only once its ready flag says so, and the claim stops at the first gap so the mark never
  // skips one. A later flush picks up what was still being written. The mark only moves forward: a
  // flush that read an older mark never pulls it back over entries another flush has already written.
  std::size_t from = flushed_upto_.load();
  std::size_t end = from;
  for (;;) {
    std::size_t count = journal_count_.load();
    if (count > kJournalCapacity) count = kJournalCapacity;
    end = from;
    while (end < count && journal_[end].ready.load(std::memory_order_acquire) != 0) ++end;
    if (end <= from) break;
    if (flushed_upto_.compare_exchange_weak(from, end)) break;  // claimed [from, end)
  }
  if (from < end) {
#if defined(NEVR_HOST_TESTING)
    claimed_.fetch_add(end - from);
#endif
    for (std::size_t at = from; at < end; ++at) {
      try {
        EmitJournalEntry(journal_[at]);
      } catch (const std::exception& error) {
        ReportBuildFailure("host_attach", error.what());
      }
    }
  }
  // Entries the full journal had no room for: one record with the running total, whenever it grew.
  const std::size_t dropped = dropped_.load();
  std::size_t reported = dropped_reported_.load();
  while (reported < dropped && !dropped_reported_.compare_exchange_weak(reported, dropped)) {
  }
  if (reported < dropped) {
    try {
      nlohmann::json record;
      record["event"] = "host_attach";
      record["step"] = StepName(AttachStep::kJournal);
      record["outcome"] = OutcomeName(AttachStep::kJournal, AttachOutcome::kFailed, AttachNote::kNone);
      record["dropped"] = dropped;
      record["capacity"] = kJournalCapacity;
      Emit(record);
    } catch (const std::exception& error) {
      ReportBuildFailure("host_attach", error.what());
    }
  }
}

void BootstrapDiagnostics::EmitJournalEntry(const JournalEntry& entry) {
  {
    nlohmann::json record;
    record["event"] = "host_attach";
    record["step"] = StepName(entry.step);
    record["outcome"] = OutcomeName(entry.step, entry.outcome, entry.note);
    const bool target_step = entry.step == AttachStep::kImageCheck ||
                             entry.step == AttachStep::kPrologueCheck ||
                             entry.step == AttachStep::kHookInstall;
    if (target_step && entry.outcome != AttachOutcome::kAccepted) {
      record["degraded_action"] = "run_original_only";
    }
    switch (entry.note) {
      case AttachNote::kHookBytesUnread:
        record["hook_bytes"] = "unread";
        record["next"] = "context_marks_the_hook_bytes_invalid";
        break;
      case AttachNote::kInstallWaitUnavailable:
      case AttachNote::kInstallWaitTimeout:
        record["next"] = "load_proceeds_unsynchronized_and_the_journal_is_flushed_again_after_load";
        break;
      case AttachNote::kDetourWithoutHook:
        record["next"] = "returned_0_without_calling_the_original";
        break;
      case AttachNote::kDuplicateSetGameModule:
        record["reason"] = "already_called";
        record["next"] = "ignored";
        break;
      case AttachNote::kSetGameModuleAfterAttach:
        record["reason"] = "attached_at_dll_main";
        record["next"] = "ignored";
        break;
      case AttachNote::kNone:
      case AttachNote::kInstallWaitSignalled:
        break;
    }
    if (entry.step == AttachStep::kHookInstall) {
      if (entry.stage != HookStage::kNone) record["stage"] = StageName(entry.stage);
      record["mh_status"] = entry.detail;
    }
    // The target the step is about: its rva, the bytes the host pinned and the bytes it found.
    if (entry.step == AttachStep::kPrologueCheck || entry.step == AttachStep::kHookInstall) {
      char rva[24];
      snprintf(rva, sizeof(rva), "0x%llx", static_cast<unsigned long long>(kGameMainWrapperRva));
      record["rva"] = rva;
      record["expected_prologue"] = HexBytes(kGameMainWrapperPrologue, kGameMainWrapperPrologueSize);
      if (entry.has_actual) {
        record["actual_prologue"] = HexBytes(entry.actual, kGameMainWrapperPrologueSize);
      }
    }
    Emit(record);
  }
}

#if defined(NEVR_HOST_TESTING)
namespace {
std::atomic<BootstrapDiagnostics::EmitSink> g_emit_sink{nullptr};
}  // namespace

void BootstrapDiagnostics::SetEmitSinkForTest(EmitSink sink) { g_emit_sink.store(sink); }
#endif

DiagnosticDestination BootstrapDiagnostics::Emit(const nlohmann::json& record) noexcept {
#if defined(NEVR_HOST_TESTING)
  if (const EmitSink sink = g_emit_sink.load()) {
    sink(record);
    return DiagnosticDestination::kPrimary;
  }
#endif
  try {
    return EmitChecked(record);
  } catch (const std::exception& error) {
    ReportBuildFailure("bootstrap_record", error.what());
    return DiagnosticDestination::kDropped;
  }
}

DiagnosticDestination BootstrapDiagnostics::EmitChecked(const nlohmann::json& record) {
  nlohmann::json stamped = record;
  char timestamp[kTimestampSize] = {};
  if (!stamped.contains("tag")) stamped["tag"] = "NEVR.HOST";
  if (!stamped.contains("ts")) {
    stamped["ts"] = FormatUtcTimestamp(timestamp) ? timestamp : "unknown";
  }
  if (!stamped.contains("pid")) stamped["pid"] = GetCurrentProcessId();

  // error_handler_t::replace: a record with invalid UTF-8 (a path from the file system) is
  // written with U+FFFD instead of throwing out of the host.
  std::string line = stamped.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  if (line.size() + 1 > kMaxRecordBytes) {
    nlohmann::json marker = {{"tag", "NEVR.HOST"},
                             {"ts", stamped.at("ts")},
                             {"pid", stamped.at("pid")},
                             {"event", record.value("event", "unknown")},
                             {"error", "record_too_large"},
                             {"record_bytes", line.size()}};
    line = marker.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }
  line.push_back('\n');
  return AppendLine(line.data(), line.size());
}

DiagnosticDestination BootstrapDiagnostics::AppendLine(const char* line,
                                                       std::size_t length) noexcept {
  if (primary_ready_ && AppendTo(primary_dir_, true, line, length)) {
    return DiagnosticDestination::kPrimary;
  }
  if (fallback_ready_ && AppendTo(fallback_dir_, true, line, length)) {
    return DiagnosticDestination::kFallback;
  }
  // The host's own directory exists already; it is not created.
  if (host_dir_ready_ && AppendTo(host_dir_, false, line, length)) {
    return DiagnosticDestination::kHostDirectory;
  }

  // Loud: a diagnostic that could not be persisted still reaches the console.
  HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
  if (stderr_handle != nullptr && stderr_handle != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(stderr_handle, line, static_cast<DWORD>(length), &written, nullptr);
  }
  return DiagnosticDestination::kDropped;
}

bool BootstrapDiagnostics::AppendTo(const wchar_t* dir, bool create_dir, const char* line,
                                    std::size_t length) noexcept {
  wchar_t path[MAX_PATH] = {};
  if (!AppendPath(path, MAX_PATH, dir, kBootstrapLogName)) return false;

  if (create_dir) {
    wchar_t dir_copy[MAX_PATH] = {};
    const std::size_t dir_len = wcslen(dir);
    if (dir_len + 1 > MAX_PATH) return false;
    std::memcpy(dir_copy, dir, (dir_len + 1) * sizeof(wchar_t));
    CreateDirectories(dir_copy);
  }

  HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  // Serialize the write only. Wait at most kBootstrapLogLockWaitMs; a wait that ends otherwise
  // writes without the lock rather than not at all.
  HANDLE lock = LogMutex();
  bool locked = false;
  if (lock != nullptr) {
    const DWORD waited = WaitForSingleObject(lock, kBootstrapLogLockWaitMs);
    locked = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
  FlushFileBuffers(file);
  if (locked) ReleaseMutex(lock);
  CloseHandle(file);
  return ok != FALSE && written == length;
}

}  // namespace nevr_host
