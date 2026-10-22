#pragma once

// Host-owned bounded diagnostic recorder for the stable BugSplat64.dll host
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md).
//
// One bounded JSON line is appended per event to nevr-bootstrap.jsonl in the
// first writable of three directories:
//   %LOCALAPPDATA%\nevr\echovr\logs
//   %TEMP%\nevr\echovr\logs
//   the directory of the host DLL
// If none is writable the line goes to stderr and the caller is told it was
// dropped; a diagnostic is never lost silently.
//
// What may run where:
//   - Prepare() copies two environment values and the host directory into fixed
//     buffers and creates the log's named mutex (one CreateMutexW call, kernel32). It does no file
//     I/O and no directory creation. That the mutex call is safe under the loader lock is an
//     inference (a named kernel object, no module load), not a measurement. The mutex is created
//     here and nowhere else; no append creates it. If the call fails, the failure is journalled
//     (step log_mutex) and appends run without the lock.
//   - RecordAttach() stores one step in a fixed journal. No heap, no I/O, no lock:
//     safe under the loader lock. Nothing is written until FlushAttachJournal().
//   - AppendLine() opens (and creates) the log file, writes once and closes, from the crash
//     callback too. It creates no mutex: it waits on the one Prepare() created. Each wait is bounded by
//     kBootstrapLogLockWaitMs; a wait that times out or fails marks the lock unreliable (one
//     host_log_lock record) and later appends in the process write without waiting. A wait that
//     ends owned is not capped per process: with a holder that releases inside every wait, every
//     destination an append opens waits once, and a record whose write fails after its wait falls
//     back to the next of up to three destinations and waits again. So one record can wait more
//     than once, and a crash of up to four records more than four times. Whether the file calls or
//     the wait allocate is not measured. No logger.
//   - Emit(), FlushAttachJournal() build records with nlohmann::json and so run
//     only after attach has returned, off the crash path.
// Nothing here calls the NEVR logger.

#include "runtime/host/game_targets.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <nlohmann/json_fwd.hpp>

namespace nevr_host {

enum class DiagnosticDestination {
  kPrimary,        // %LOCALAPPDATA%\nevr\echovr\logs
  kFallback,       // %TEMP%\nevr\echovr\logs
  kHostDirectory,  // next to BugSplat64.dll
  kDropped,        // no directory was writable (the line went to stderr)
};

/// Stable lower-case token for a destination, as written in records.
const char* DestinationName(DiagnosticDestination destination) noexcept;

/// One decision the host makes while attaching, in the order it makes them.
enum class AttachStep : std::uint32_t {
  kImageCheck = 1,     // is the process image a supported echovr.exe
  kPrologueCheck = 2,  // does GameMainWrapper still start with the pinned bytes
  kHookInstall = 3,    // MinHook initialize, create and enable
  kInstallWait = 4,    // the load waited for the install to finish journalling
  kDetourEntry = 5,    // the detour was entered in a state it must never be entered in
  kSetGameModule = 6,  // a second NEVR_SetGameModule
  kJournal = 7,        // the journal itself: entries it had no room for
  kLogMutex = 8,       // the log's mutex could not be created: appends run without the lock
};

/// What an entry adds to its step: the outcome of a wait, the reason for a refusal, a fact about
/// the install. Each names what the host did next where that is not obvious.
enum class AttachNote : std::uint32_t {
  kNone = 0,
  kHookBytesUnread = 1,          // hook_install: the hook is live but its first bytes were not read
  kInstallWaitSignalled = 2,     // install_wait: the install had finished
  kInstallWaitUnavailable = 3,   // install_wait: no event to wait on (CreateEventW failed)
  kInstallWaitTimeout = 4,       // install_wait: the install did not finish in time
  kDetourWithoutHook = 5,        // detour_entry: entered with no trampoline published
  kDuplicateSetGameModule = 6,   // set_game_module: already called once
  kSetGameModuleAfterAttach = 7, // set_game_module: DllMain already attached to the process image
  kLogMutexUnavailable = 8,      // log_mutex: CreateMutexW failed (detail carries the Win32 error)
};

/// The MinHook call a hook-install step reached (the one that failed, or enable on success).
enum class HookStage : std::uint32_t {
  kNone = 0,
  kInitialize = 1,  // MH_Initialize
  kCreate = 2,      // MH_CreateHook
  kEnable = 3,      // MH_EnableHook
};

enum class AttachOutcome : std::uint32_t {
  kAccepted = 1,  // image or prologue matched; hook enabled
  kRejected = 2,  // image or prologue did not match
  kFailed = 3,    // MinHook returned an error (detail carries MH_STATUS)
};

class BootstrapDiagnostics {
 public:
  /// Copy %LOCALAPPDATA%, %TEMP% and the directory of `host_module`, compute the three log
  /// directories and create the log mutex (once per process). No file I/O.
  void Prepare(HMODULE host_module);

  /// Store one attach step. `actual` (optional, kGameMainWrapperPrologueSize long) is the
  /// prologue found at the target, kept so a mismatch can be explained. `detail` is the
  /// MH_STATUS of a hook-install step and `stage` the MinHook call it belongs to.
  void RecordAttach(AttachStep step, AttachOutcome outcome, std::uint32_t detail = 0,
                    const unsigned char* actual = nullptr, HookStage stage = HookStage::kNone,
                    AttachNote note = AttachNote::kNone) noexcept;

  /// Write every recorded attach step not yet written (a call with nothing new is a
  /// no-op). Called after
  /// attach returns: from the first GameMainWrapper call, and from the host
  /// entry points that fire without the detour (the crash-reporter constructor and
  /// NEVR_SetGameModule), so a failed attach is reported when the game constructs its
  /// crash reporter even though no detour exists to trigger it earlier.
  void FlushAttachJournal() noexcept;

  /// The last flush: the process is ending (DLL_PROCESS_DETACH with lpReserved != NULL), so every other thread
  /// is already dead. Writes each recorded step not yet written, including those a flush thread claimed and was
  /// killed before it wrote, the failing steps first. Each record carries its journal index as `seq` (with the
  /// record's `pid`, one key per step): a claimed step is looked up in the log before it is written again. The
  /// wait on the log mutex, which a live process on the same prefix can hold, is at most
  /// kBootstrapExitLogLockWaitMs; when it is not taken the flush writes nothing (an unlocked line could
  /// interleave with the holder's) and stops. It takes no other lock. A step already written is not written again.
  void FlushAttachJournalAtExit() noexcept;

  /// Name the thread that is inside the host's DllMain attach (0: none). A flush that runs on it is a defect (a
  /// file cannot be written under the loader lock) and is counted, so a test can fail on it.
  static void MarkAttachThread(DWORD thread_id) noexcept;
  static std::size_t FlushesOnAttachThread() noexcept;

  /// True when any recorded attach step was a rejection or a failure, an install record says its
  /// hook bytes were unread, or the journal dropped entries.
  bool AttachDegraded() const noexcept;

  /// Append one record built with nlohmann::json. `tag`, `ts` and `pid` are added.
  /// A record larger than kMaxRecordBytes is replaced by a marker record rather
  /// than truncated.
  /// Never throws: a failure to build the record is reported on stderr and returned as
  /// kDropped.
  DiagnosticDestination Emit(const nlohmann::json& record) noexcept;

  /// Append an already-formatted line (ends in '\n'), also from the crash callback. It creates no
  /// mutex; its wait on the log mutex is bounded by kBootstrapLogLockWaitMs, and a wait that does not end
  /// owned makes later appends skip the wait (see the file comment).
  DiagnosticDestination AppendLine(const char* line, std::size_t length) noexcept;

  static constexpr std::size_t kMaxRecordBytes = 4096;
  static constexpr std::uint32_t kEntryClaimed = 1;  // a flush took the entry and may not have finished its write
  static constexpr std::uint32_t kEntryWritten = 2;
  static constexpr std::size_t kJournalCapacity = 8;

  // Read-only accessors.
  const wchar_t* PrimaryDir() const { return primary_dir_; }
  const wchar_t* FallbackDir() const { return fallback_dir_; }
  const wchar_t* HostDir() const { return host_dir_; }
  bool PrimaryReady() const { return primary_ready_; }
  bool FallbackReady() const { return fallback_ready_; }
  bool HostDirReady() const { return host_dir_ready_; }
  std::size_t JournalFlushed() const { return flushed_upto_.load(); }
  std::size_t JournalSize() const { return journal_count_.load(); }
  std::size_t JournalDropped() const { return dropped_.load(); }
#if defined(NEVR_HOST_TESTING)
  /// Test seam: set the three directories directly instead of reading the environment
  /// and the module path. An empty string leaves that destination unavailable. Also creates the log
  /// mutex and clears the "lock timed out" mark, as Prepare does at attach.
  void PrepareForTest(const wchar_t* local_app_data, const wchar_t* temp,
                      const wchar_t* host_dir);

  /// Close the log mutex and forget it and clear the timed-out mark, so a test can show what an append
  /// does when no mutex exists (a crash before the first Prepare).
  static void ResetLogLockForTest();

  /// Name the log mutex `name` for the next CreateLogMutex (null or empty: the production name), so a
  /// test does not share the production mutex with another test process. Call ResetLogLockForTest first.
  static void SetLogMutexNameForTest(const wchar_t* name);

  /// True once an append's wait on the log mutex timed out.
  static bool LogLockUnreliableForTest();

  /// Entries flushes have claimed to write, over all flushes. Equal to the entries recorded once every
  /// flush has returned; more means an entry was claimed twice.
  std::size_t JournalClaimed() const;

  /// Set an entry's flush state (0, kEntryClaimed, kEntryWritten): a flush thread killed between the write and the mark.
  void SetEntryStateForTest(std::size_t at, std::uint32_t state) { journal_[at].written.store(state); }

  /// Appends the last flush skipped because the log mutex was held (by another process) past its wait.
  static std::size_t ExitFlushSkippedForTest();

  /// Replace the output: every record Emit would write goes to `sink` instead. Null restores the file.
  using EmitSink = void (*)(const nlohmann::json& record);
  static void SetEmitSinkForTest(EmitSink sink);

  /// Run `pause` in the middle of RecordAttach, after the slot is reserved and part of it written:
  /// the window in which a flush must not read the entry. Null removes it.
  using RecordPause = void (*)();
  static void SetRecordPauseForTest(RecordPause pause);
#endif

 private:
  struct JournalEntry {
    AttachStep step;
    AttachOutcome outcome;
    std::uint32_t detail;
    HookStage stage;
    AttachNote note;
    bool has_actual;
    unsigned char actual[kGameMainWrapperPrologueSize];
    // Stored last, with release ordering: RecordAttach reserves a slot before it fills it, so a flush
    // must not read an entry until this says it is complete.
    std::atomic<std::uint32_t> ready;
    // 0: not taken by a flush; kEntryClaimed: a flush is writing it, or was killed doing so; kEntryWritten.
    std::atomic<std::uint32_t> written;
  };

  void EmitJournalEntry(const JournalEntry& entry);
  void EmitDroppedTotal() noexcept;
  bool EntryIsInLog(std::size_t seq) const noexcept;
  void NoteFlushCaller() noexcept;
  DiagnosticDestination EmitChecked(const nlohmann::json& record);
  void EnsureLogMutex() noexcept;
  bool AppendTo(const wchar_t* dir, bool create_dir, const char* line, std::size_t length) noexcept;

  wchar_t local_app_data_[MAX_PATH] = {};
  wchar_t temp_[MAX_PATH] = {};
  wchar_t primary_dir_[MAX_PATH] = {};   // %LOCALAPPDATA%\nevr\echovr\logs
  wchar_t fallback_dir_[MAX_PATH] = {};  // %TEMP%\nevr\echovr\logs
  wchar_t host_dir_[MAX_PATH] = {};      // directory of BugSplat64.dll
  bool primary_ready_ = false;
  bool fallback_ready_ = false;
  bool host_dir_ready_ = false;
  JournalEntry journal_[kJournalCapacity] = {};
  std::atomic<std::size_t> journal_count_{0};
  std::atomic<std::size_t> flushed_upto_{0};
#if defined(NEVR_HOST_TESTING)
  std::atomic<std::size_t> claimed_{0};
#endif
  std::atomic<std::size_t> dropped_{0};           // entries the full journal had no room for
  std::atomic<std::size_t> dropped_reported_{0};  // the total the last overflow record carried
};

}  // namespace nevr_host
