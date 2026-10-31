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
    case AttachStep::kLogMutex: return "log_mutex";
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
    case AttachStep::kLogMutex: return "unavailable";
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

#if defined(NEVR_HOST_TESTING)
std::atomic<BootstrapDiagnostics::EmitSink> g_emit_sink{nullptr};
std::atomic<BootstrapDiagnostics::RecordPause> g_record_pause{nullptr};
#endif

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

// The log's mutex. It is created once, by CreateLogMutex() from Prepare() (host attach), and kept for
// the process. The append path, which the crash callback also runs, only loads the handle: it never
// creates the mutex. Null when Prepare() has not run or could not create it (appends then run
// unserialized, and Prepare journalled why).
std::atomic<HANDLE> g_log_mutex{nullptr};

// Set by the first append whose wait on the mutex does not end owned (it timed out or failed). Later
// appends in the process write without waiting, so a holder that never releases costs one wait, not
// one per record. Waits that end owned are not counted: each is bounded by kBootstrapLogLockWaitMs and
// there is no per-process budget.
std::atomic<bool> g_log_lock_unreliable{false};

// Writes that skipped the mutex because it was marked unreliable. Read by the concurrent-append test when a line
// is missing (#840): with it and the host_log_lock record, a short file says whether the lock path ran.
std::atomic<unsigned long long> g_log_unlocked_writes{0};

// How long an append waits on the log mutex.
std::atomic<DWORD> g_log_wait_ms{kBootstrapLogLockWaitMs};

// How many times an append tries to open the log file before it gives up on that destination (#733).
constexpr int kAppendOpenAttempts = 4;

// Serializes the writes of this process's appenders once the named mutex's wait has ended without it. An append
// that timed out on the mutex writes unlocked; two such writes from one process to the same FILE_APPEND_DATA
// handle race in the file system (under Wine a line is reported written and is absent from the file, #997). The
// critical section is recursive for its owner, so the crash callback on the thread that holds it still writes at
// once, and it is taken after the mutex's bounded wait, so no path waits on it while it waits on the mutex.
CRITICAL_SECTION g_log_local_lock;
// Initialized once, on the first append, by InitOnceExecuteOnce: two threads that log for the first time together
// run the callback once, and each waits for it to finish. kernel32 only, so it is safe where the loader lock is held.
INIT_ONCE g_log_local_lock_once = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK InitLogLocalLock(PINIT_ONCE, PVOID, PVOID*) noexcept {
  InitializeCriticalSection(&g_log_local_lock);
  return TRUE;
}

// The last flush of a process that is ending (FlushAttachJournalAtExit). The log mutex is named, so a live process on
// the same prefix can hold it. The exit flush waits at most kBootstrapExitLogLockWaitMs for it and, when it does not
// get it, writes nothing: an unlocked write could interleave with a live process's line. After the first such
// failure the rest of the exit flush skips at once.
std::atomic<bool> g_exit_flush{false};
std::atomic<bool> g_exit_lock_lost{false};
std::atomic<std::size_t> g_exit_skipped{0};

// The run token: 64 bits that name this process incarnation, stamped on every record as `run` (16 hex digits). A pid
// is not enough: Wine hands out the same pids launch after launch (the game is pid 32 every time), so a reader and the
// exit flush key a journal record by (run, seq). It is the process's creation time, its pid and the performance counter
// at the first call, mixed; no heap, no I/O, kernel32 only, so it is safe under the loader lock. Zero means "not yet
// computed". A test can set it to stand for another process.
std::atomic<std::uint64_t> g_run_token{0};

std::uint64_t MixToken(std::uint64_t value) noexcept {  // splitmix64 finalizer
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint64_t RunToken() noexcept {
  const std::uint64_t token = g_run_token.load(std::memory_order_acquire);
  if (token != 0) return token;
  FILETIME created = {};
  FILETIME exited = {};
  FILETIME kernel = {};
  FILETIME user = {};
  GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
  LARGE_INTEGER counter = {};
  QueryPerformanceCounter(&counter);
  std::uint64_t made = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  made = MixToken(made ^ MixToken(static_cast<std::uint64_t>(GetCurrentProcessId())) ^
                  MixToken(static_cast<std::uint64_t>(counter.QuadPart)));
  if (made == 0) made = 1;
  std::uint64_t expected = 0;
  g_run_token.compare_exchange_strong(expected, made, std::memory_order_acq_rel);
  return g_run_token.load(std::memory_order_acquire);
}

// The thread inside the host's DllMain attach, and how many flushes ran on it.
std::atomic<DWORD> g_attach_thread{0};
std::atomic<std::size_t> g_flushes_on_attach_thread{0};

#if defined(NEVR_HOST_TESTING)
wchar_t g_log_mutex_name_for_test[128] = {};  // empty: the production name
#endif

const wchar_t* LogMutexName() noexcept {
#if defined(NEVR_HOST_TESTING)
  if (g_log_mutex_name_for_test[0] != L'\0') return g_log_mutex_name_for_test;
#endif
  return kBootstrapLogMutexName;
}

// Create the mutex unless it exists. Returns 0, or the Win32 error CreateMutexW failed with.
DWORD CreateLogMutex() noexcept {
  if (g_log_mutex.load(std::memory_order_acquire) != nullptr) return 0;
  HANDLE created = CreateMutexW(nullptr, FALSE, LogMutexName());
  if (created == nullptr) {
    const DWORD error = GetLastError();
    return error != 0 ? error : ERROR_GEN_FAILURE;
  }
  HANDLE expected = nullptr;
  if (!g_log_mutex.compare_exchange_strong(expected, created)) CloseHandle(created);
  return 0;
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
  // The log's mutex is created here, at attach, and never inside an append: a crash callback that is
  // the first to write must not have to create it. CreateMutexW is a kernel32 call. That it is safe under
  // the loader lock is an inference (it opens a named kernel object and loads no module), not a measurement.
  EnsureLogMutex();
}

void BootstrapDiagnostics::EnsureLogMutex() noexcept {
  const DWORD error = CreateLogMutex();
  if (error != 0) {
    // Nothing is written here (the loader lock may be held): the journal says it when it is flushed.
    RecordAttach(AttachStep::kLogMutex, AttachOutcome::kFailed, error, nullptr, HookStage::kNone,
                 AttachNote::kLogMutexUnavailable);
  }
}

#if defined(NEVR_HOST_TESTING)
void BootstrapDiagnostics::SetRunTokenForTest(std::uint64_t token) { g_run_token.store(token); }
std::uint64_t BootstrapDiagnostics::RunTokenForTest() { return RunToken(); }

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
  g_log_lock_unreliable.store(false);  // a test starts with a lock that has not yet timed out
  EnsureLogMutex();
}

void BootstrapDiagnostics::ResetLogLockForTest() {
  g_log_lock_unreliable.store(false);
  HANDLE old = g_log_mutex.exchange(nullptr);
  if (old != nullptr) CloseHandle(old);
}

void BootstrapDiagnostics::SetLogMutexNameForTest(const wchar_t* name) {
  g_log_mutex_name_for_test[0] = L'\0';
  if (name == nullptr || name[0] == L'\0') return;
  const std::size_t length = wcslen(name);
  if (length + 1 > sizeof(g_log_mutex_name_for_test) / sizeof(wchar_t)) return;
  std::memcpy(g_log_mutex_name_for_test, name, (length + 1) * sizeof(wchar_t));
}

bool BootstrapDiagnostics::LogLockUnreliableForTest() { return g_log_lock_unreliable.load(); }
unsigned long long BootstrapDiagnostics::UnlockedWritesForTest() { return g_log_unlocked_writes.load(); }
std::size_t BootstrapDiagnostics::JournalClaimed() const { return claimed_.load(); }
#endif

void BootstrapDiagnostics::RecordAttach(AttachStep step, AttachOutcome outcome,
                                        std::uint32_t detail, const unsigned char* actual,
                                        HookStage stage, AttachNote note) noexcept {
  // The first step that left the game without the runtime, kept for the stop line (stop_line.h). Stored before the slot
  // is reserved, so a journal that is full cannot lose it. An atomic and nothing else: this runs under the loader lock.
  if (outcome != AttachOutcome::kAccepted) {
    AttachFailure failure = AttachFailure::kNone;
    switch (step) {
      case AttachStep::kImageCheck: failure = AttachFailure::kImageCheck; break;
      case AttachStep::kPrologueCheck: failure = AttachFailure::kPrologueCheck; break;
      case AttachStep::kHookInstall: failure = AttachFailure::kHookInstall; break;
      case AttachStep::kInstallWait:
      case AttachStep::kDetourEntry:
      case AttachStep::kSetGameModule:
      case AttachStep::kJournal:
      case AttachStep::kLogMutex:
        break;
    }
    if (failure != AttachFailure::kNone) {
      std::uint32_t none = 0;
      first_failure_.compare_exchange_strong(none, static_cast<std::uint32_t>(failure));
    }
  }
  const std::size_t slot = journal_count_.fetch_add(1);
  if (slot >= kJournalCapacity) {
    journal_count_.store(kJournalCapacity);  // saturate: the first entries are the ones kept
    dropped_.fetch_add(1);                   // and the rest are counted, then reported at the flush
    return;
  }
  JournalEntry& entry = journal_[slot];
  entry.step = step;
  entry.outcome = outcome;
#if defined(NEVR_HOST_TESTING)
  if (const RecordPause pause = g_record_pause.load()) pause();
#endif
  entry.detail = detail;
  entry.stage = stage;
  entry.note = note;
  entry.has_actual = actual != nullptr;
  if (actual != nullptr) std::memcpy(entry.actual, actual, kGameMainWrapperPrologueSize);
  entry.ready.store(1, std::memory_order_release);
}

AttachFailure BootstrapDiagnostics::FirstAttachFailure() const noexcept {
  return static_cast<AttachFailure>(first_failure_.load());
}

bool BootstrapDiagnostics::LogFilePath(wchar_t* out, std::size_t size) const noexcept {
  if (out == nullptr || size == 0) return false;
  out[0] = L'\0';
  const wchar_t* dir = nullptr;
  const int last = last_destination_.load();
  if (last == -1) {
    // Nothing written yet: the first directory an append will try.
    dir = primary_ready_ ? primary_dir_ : (fallback_ready_ ? fallback_dir_ : (host_dir_ready_ ? host_dir_ : nullptr));
  } else if (last == static_cast<int>(DiagnosticDestination::kPrimary)) {
    dir = primary_dir_;
  } else if (last == static_cast<int>(DiagnosticDestination::kFallback)) {
    dir = fallback_dir_;
  } else if (last == static_cast<int>(DiagnosticDestination::kHostDirectory)) {
    dir = host_dir_;
  }  // kDropped: the last record reached no file
  if (dir == nullptr) return false;
  return AppendPath(out, size, dir, kBootstrapLogName);
}

bool BootstrapDiagnostics::AttachDegraded() const noexcept {
  const std::size_t count = journal_count_.load();
  for (std::size_t at = 0; at < count && at < kJournalCapacity; ++at) {
    if (journal_[at].ready.load(std::memory_order_acquire) == 0) continue;
    if (journal_[at].step == AttachStep::kLogMutex) continue;  // the log runs unserialized; the attach is intact
    if (journal_[at].outcome != AttachOutcome::kAccepted) return true;
    if (journal_[at].note == AttachNote::kHookBytesUnread) return true;
  }
  return dropped_.load() != 0;
}

void BootstrapDiagnostics::MarkAttachThread(DWORD thread_id) noexcept { g_attach_thread.store(thread_id); }

std::size_t BootstrapDiagnostics::FlushesOnAttachThread() noexcept { return g_flushes_on_attach_thread.load(); }

#if defined(NEVR_HOST_TESTING)
std::size_t BootstrapDiagnostics::ExitFlushSkippedForTest() { return g_exit_skipped.load(); }
#endif

void BootstrapDiagnostics::NoteFlushCaller() noexcept {
  const DWORD attach_thread = g_attach_thread.load();
  if (attach_thread != 0 && attach_thread == GetCurrentThreadId()) g_flushes_on_attach_thread.fetch_add(1);
}

void BootstrapDiagnostics::FlushAttachJournal() noexcept {
  NoteFlushCaller();
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
      // Claimed before the write: a process that ends while this thread is in the write leaves the entry
      // "claimed", and the last flush then looks in the log for it before it writes it again.
      journal_[at].written.store(kEntryClaimed, std::memory_order_release);
      try {
        EmitJournalEntry(journal_[at]);
      } catch (const std::exception& error) {
        ReportBuildFailure("host_attach", error.what());
      }
      journal_[at].written.store(kEntryWritten, std::memory_order_release);
    }
  }
  EmitDroppedTotal();
}

void BootstrapDiagnostics::FlushAttachJournalAtExit() noexcept {
  NoteFlushCaller();
  // Other threads are dead, so nothing writes while this runs and no entry is half-written by a live thread: an
  // entry that is not "written" is one a flush claimed and did not finish, or never reached. A claimed entry may
  // have reached the file before its thread was killed, so the log is read for its (run, seq) first. The failing
  // steps go first: they are the reason the log is read.
  g_exit_lock_lost.store(false);
  g_exit_skipped.store(0);
  g_exit_flush.store(true);
  std::size_t count = journal_count_.load();
  if (count > kJournalCapacity) count = kJournalCapacity;
  for (int pass = 0; pass < 2 && !g_exit_lock_lost.load(); ++pass) {
    for (std::size_t at = 0; at < count && !g_exit_lock_lost.load(); ++at) {
      JournalEntry& entry = journal_[at];
      if (entry.ready.load(std::memory_order_acquire) == 0) continue;
      if ((entry.outcome != AttachOutcome::kAccepted) != (pass == 0)) continue;
      const std::uint32_t state = entry.written.load(std::memory_order_acquire);
      if (state == kEntryWritten) continue;
      if (state == kEntryClaimed && EntryIsInLog(at)) {
        entry.written.store(kEntryWritten, std::memory_order_release);
        continue;
      }
      entry.written.store(kEntryClaimed, std::memory_order_release);
      try {
        EmitJournalEntry(entry);
      } catch (const std::exception& error) {
        ReportBuildFailure("host_attach", error.what());
      }
      if (!g_exit_lock_lost.load()) entry.written.store(kEntryWritten, std::memory_order_release);
    }
  }
  flushed_upto_.store(count);
  if (!g_exit_lock_lost.load()) EmitDroppedTotal();
  g_exit_flush.store(false);
}

bool BootstrapDiagnostics::EntryIsInLog(std::size_t seq) const noexcept {
  // The last part of each log file: the records of this process are at its end. Reads take no lock.
  const wchar_t* const dirs[3] = {primary_ready_ ? primary_dir_ : nullptr, fallback_ready_ ? fallback_dir_ : nullptr,
                                  host_dir_ready_ ? host_dir_ : nullptr};
  char run_key[48];
  char seq_key[40];
  snprintf(run_key, sizeof(run_key), "\"run\":\"%016llx\"", static_cast<unsigned long long>(RunToken()));
  snprintf(seq_key, sizeof(seq_key), "\"seq\":%llu", static_cast<unsigned long long>(seq));
  for (const wchar_t* dir : dirs) {
    if (dir == nullptr) continue;
    wchar_t path[MAX_PATH] = {};
    if (!AppendPath(path, MAX_PATH, dir, kBootstrapLogName)) continue;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    LARGE_INTEGER size = {};
    char buffer[64 * 1024 + 1];
    DWORD got = 0;
    bool found = false;
    if (GetFileSizeEx(file, &size)) {
      LARGE_INTEGER at = {};
      at.QuadPart = size.QuadPart > static_cast<LONGLONG>(sizeof(buffer) - 1)
                        ? size.QuadPart - static_cast<LONGLONG>(sizeof(buffer) - 1)
                        : 0;
      if (SetFilePointerEx(file, at, nullptr, FILE_BEGIN) &&
          ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer) - 1), &got, nullptr)) {
        buffer[got] = '\0';
        // Line by line: the keys must be on one line, and it must be a journal record.
        for (char* line = buffer; line < buffer + got;) {
          char* end = std::strchr(line, '\n');
          if (end != nullptr) *end = '\0';
          if (std::strstr(line, "\"event\":\"host_attach\"") != nullptr && std::strstr(line, run_key) != nullptr &&
              std::strstr(line, seq_key) != nullptr) {
            // "seq":1 must not match "seq":12: the next character is not a digit.
            const char* hit = std::strstr(line, seq_key);
            const char next = hit[std::strlen(seq_key)];
            if (next < '0' || next > '9') {
              found = true;
              break;
            }
          }
          if (end == nullptr) break;
          line = end + 1;
        }
      }
    }
    CloseHandle(file);
    if (found) return true;
  }
  return false;
}

void BootstrapDiagnostics::EmitDroppedTotal() noexcept {
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
    // With the record's `run` (stamped by Emit), the key of this entry: a reader keeps one record per (run, seq).
    // The pid is not part of the key, since pids are reused.
    record["seq"] = static_cast<std::size_t>(&entry - journal_);
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
      case AttachNote::kLogMutexUnavailable:
        record["win32_error"] = entry.detail;
        record["next"] = "log_appends_run_without_the_lock";
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
void BootstrapDiagnostics::SetEmitSinkForTest(EmitSink sink) { g_emit_sink.store(sink); }
void BootstrapDiagnostics::SetRecordPauseForTest(RecordPause pause) { g_record_pause.store(pause); }
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

namespace {

std::string PathToUtf8(const wchar_t* path) {
  const int needed = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), needed, nullptr, nullptr);
  out.resize(static_cast<std::size_t>(needed) - 1);
  return out;
}

// One record as one line: invalid UTF-8 (a path from the file system) is written as U+FFFD instead of throwing out of
// the host, and a record over the limit is replaced by a marker that keeps its identity.
std::string SerializeRecord(const nlohmann::json& stamped) {
  std::string line = stamped.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  if (line.size() + 1 > BootstrapDiagnostics::kMaxRecordBytes) {
    nlohmann::json marker = {{"tag", "NEVR.HOST"},
                             {"ts", stamped.at("ts")},
                             {"pid", stamped.at("pid")},
                             {"run", stamped.at("run")},
                             {"event", stamped.value("event", "unknown")},
                             {"error", "record_too_large"},
                             {"record_bytes", line.size()}};
    line = marker.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }
  line.push_back('\n');
  return line;
}

}  // namespace

DiagnosticDestination BootstrapDiagnostics::EmitChecked(const nlohmann::json& record) {
  nlohmann::json stamped = record;
  char timestamp[kTimestampSize] = {};
  if (!stamped.contains("tag")) stamped["tag"] = "NEVR.HOST";
  if (!stamped.contains("ts")) {
    stamped["ts"] = FormatUtcTimestamp(timestamp) ? timestamp : "unknown";
  }
  if (!stamped.contains("pid")) stamped["pid"] = GetCurrentProcessId();
  if (!stamped.contains("run")) {
    char run[17] = {};
    snprintf(run, sizeof(run), "%016llx", static_cast<unsigned long long>(RunToken()));
    stamped["run"] = run;
  }

  // A record that carries a string `log` (the stop facts, stop_line.h) names the file it is written to: the field is
  // set per directory tried, so a record that fell back to %TEMP% does not name the primary directory.
  if (stamped.contains("log") && stamped.at("log").is_string()) return AppendRecordNamingLog(stamped);

  const std::string line = SerializeRecord(stamped);
  return AppendLine(line.data(), line.size());
}

DiagnosticDestination BootstrapDiagnostics::AppendRecordNamingLog(nlohmann::json& stamped) {
  struct Candidate {
    const wchar_t* dir;
    bool create_dir;
    bool ready;
    DiagnosticDestination destination;
  };
  const Candidate candidates[] = {
      {primary_dir_, true, primary_ready_, DiagnosticDestination::kPrimary},
      {fallback_dir_, true, fallback_ready_, DiagnosticDestination::kFallback},
      {host_dir_, false, host_dir_ready_, DiagnosticDestination::kHostDirectory},
  };
  for (const Candidate& candidate : candidates) {
    if (!candidate.ready) continue;
    wchar_t path[MAX_PATH] = {};
    if (!AppendPath(path, MAX_PATH, candidate.dir, kBootstrapLogName)) continue;
    stamped["log"] = PathToUtf8(path);
    const std::string line = SerializeRecord(stamped);
    if (AppendTo(candidate.dir, candidate.create_dir, line.data(), line.size())) {
      last_destination_.store(static_cast<int>(candidate.destination));
      return candidate.destination;
    }
  }
  stamped["log"] = "";
  const std::string line = SerializeRecord(stamped);
  return ReportDropped(line.data(), line.size());
}

DiagnosticDestination BootstrapDiagnostics::AppendLine(const char* line,
                                                       std::size_t length) noexcept {
  if (primary_ready_ && AppendTo(primary_dir_, true, line, length)) {
    last_destination_.store(static_cast<int>(DiagnosticDestination::kPrimary));
    return DiagnosticDestination::kPrimary;
  }
  if (fallback_ready_ && AppendTo(fallback_dir_, true, line, length)) {
    last_destination_.store(static_cast<int>(DiagnosticDestination::kFallback));
    return DiagnosticDestination::kFallback;
  }
  // The host's own directory exists already; it is not created.
  if (host_dir_ready_ && AppendTo(host_dir_, false, line, length)) {
    last_destination_.store(static_cast<int>(DiagnosticDestination::kHostDirectory));
    return DiagnosticDestination::kHostDirectory;
  }
  return ReportDropped(line, length);
}

DiagnosticDestination BootstrapDiagnostics::ReportDropped(const char* line, std::size_t length) noexcept {
  last_destination_.store(static_cast<int>(DiagnosticDestination::kDropped));

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
  if (!InitOnceExecuteOnce(&g_log_local_lock_once, InitLogLocalLock, nullptr, nullptr)) return false;
  wchar_t path[MAX_PATH] = {};
  if (!AppendPath(path, MAX_PATH, dir, kBootstrapLogName)) return false;

  if (create_dir) {
    wchar_t dir_copy[MAX_PATH] = {};
    const std::size_t dir_len = wcslen(dir);
    if (dir_len + 1 > MAX_PATH) return false;
    std::memcpy(dir_copy, dir, (dir_len + 1) * sizeof(wchar_t));
    CreateDirectories(dir_copy);
  }

  const bool exiting = g_exit_flush.load();
  if (exiting && g_exit_lock_lost.load() && g_log_mutex.load(std::memory_order_acquire) != nullptr) return false;
  // An open refused while other appenders have the file open (a sharing violation under load) is retried at once,
  // a bounded number of times: a line whose open fails here is lost from the file.
  HANDLE file = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < kAppendOpenAttempts && file == INVALID_HANDLE_VALUE; ++attempt) {
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (file == INVALID_HANDLE_VALUE) return false;

  // Serialize the write only, with the mutex Prepare() created (none: the write is unserialized).
  // Each wait is at most kBootstrapLogLockWaitMs. The first wait that does not end owned marks the
  // lock unreliable, and from then on appends in the process write without waiting.
  HANDLE lock = g_log_mutex.load(std::memory_order_acquire);
  bool locked = false;
  bool marked_unreliable = false;
  DWORD waited = WAIT_OBJECT_0;
  DWORD wait_error = 0;
  if (exiting && lock != nullptr) {
    // The last flush: a bounded wait, then no write. The unreliable mark of earlier appends does not apply: it
    // was set by a wait in this process, and the holder may be another one.
    waited = WaitForSingleObject(lock, kBootstrapExitLogLockWaitMs);
    locked = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    if (!locked) {
      g_exit_lock_lost.store(true);
      g_exit_skipped.fetch_add(1);
      CloseHandle(file);
      return false;
    }
  } else if (lock != nullptr && !g_log_lock_unreliable.load()) {
    waited = WaitForSingleObject(lock, g_log_wait_ms.load());
    if (waited == WAIT_FAILED) wait_error = GetLastError();
    locked = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    if (!locked) marked_unreliable = !g_log_lock_unreliable.exchange(true);
  } else if (lock != nullptr) {
    g_log_unlocked_writes.fetch_add(1);
  }
  // The write is serialized within the process whether or not the mutex was taken: a write that did not get the mutex
  // must not race another write of this process (#997).
  EnterCriticalSection(&g_log_local_lock);
  // A short write is continued from where it stopped, so a line is never left without its tail.
  DWORD written = 0;
  BOOL ok = TRUE;
  while (written < length && ok) {
    DWORD chunk = 0;
    ok = WriteFile(file, line + written, static_cast<DWORD>(length) - written, &chunk, nullptr);
    if (chunk == 0) break;
    written += chunk;
  }
  if (marked_unreliable) {
    // The one record that says the log is no longer serialized: written to the open file, without the lock,
    // from a fixed buffer (this can be the crash callback).
    char buffer[512];
    JsonLine record(buffer, sizeof(buffer), "host_log_lock");
    record.Str("stage", "wait").Str("outcome", waited == WAIT_FAILED ? "failed" : "timeout");
    record.UInt("wait_result", waited).UInt("wait_ms", g_log_wait_ms.load());
    if (wait_error != 0) record.UInt("win32_error", wait_error);
    record.Str("next", "later_appends_write_without_the_lock");
    const std::size_t record_length = record.Finish();
    DWORD record_written = 0;
    if (record_length != 0) {
      // Written inside the in-process section. Written after LeaveCriticalSection it raced another thread's line on the
      // same file, a thread that holds neither the mutex (the lock is unreliable) nor this section.
      WriteFile(file, buffer, static_cast<DWORD>(record_length), &record_written, nullptr);
    }
  }
  LeaveCriticalSection(&g_log_local_lock);
  if (locked) ReleaseMutex(lock);
  // The flush is after the release, not inside the wait's hold: FlushFileBuffers is the slow step, and holding the
  // named mutex across it made processes that end together queue for the bounded exit wait behind each other's fsync
  // (#1152). The line and the lock record are already in the file; the file is flushed before this call returns, so the
  // process is not allowed to end before its records reach the disk.
  FlushFileBuffers(file);
  CloseHandle(file);
  return ok != FALSE && written == length;
}

}  // namespace nevr_host
