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
//     buffers. No heap, no I/O, no directory creation: safe under the loader lock.
//   - RecordAttach() stores one step in a fixed journal. No heap, no I/O, no lock:
//     safe under the loader lock. Nothing is written until FlushAttachJournal().
//   - AppendLine() opens, writes once and closes. No heap, no locks, no logger:
//     safe in the crash callback.
//   - Emit(), FlushAttachJournal() build records with nlohmann::json and so run
//     only after attach has returned, off the crash path.
// Nothing here calls the NEVR logger.

#include "runtime/host/preprocess_target.h"

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
  kPrologueCheck = 2,  // does PreprocessCommandLine still start with the pinned bytes
  kHookInstall = 3,    // MinHook initialize, create and enable
};

enum class AttachOutcome : std::uint32_t {
  kAccepted = 1,  // image or prologue matched; hook enabled
  kRejected = 2,  // image or prologue did not match
  kFailed = 3,    // MinHook returned an error (detail carries MH_STATUS)
};

class BootstrapDiagnostics {
 public:
  /// Copy %LOCALAPPDATA%, %TEMP% and the directory of `host_module` and compute the
  /// three log directories. No allocation, no I/O.
  void Prepare(HMODULE host_module);

  /// Test seam: set the three directories directly instead of reading the environment
  /// and the module path. An empty string leaves that destination unavailable.
  void PrepareForTest(const wchar_t* local_app_data, const wchar_t* temp,
                      const wchar_t* host_dir);

  /// Store one attach step. `actual` (optional, kPreprocessPrologueSize long) is the
  /// prologue found at the target, kept so a mismatch can be explained.
  void RecordAttach(AttachStep step, AttachOutcome outcome, std::uint32_t detail = 0,
                    const unsigned char* actual = nullptr) noexcept;

  /// Write every recorded attach step not yet written (a call with nothing new is a
  /// no-op). Called after
  /// attach returns: from the first PreprocessCommandLine call, and from the host
  /// entry points that fire without the detour (the crash-reporter constructor and
  /// NEVR_SetGameModule), so a failed attach is reported when the game constructs its
  /// crash reporter even though no detour exists to trigger it earlier.
  void FlushAttachJournal() noexcept;

  /// True when any recorded attach step was a rejection or a failure.
  bool AttachDegraded() const noexcept;

  /// Append one record built with nlohmann::json. `tag`, `ts` and `pid` are added.
  /// A record larger than kMaxRecordBytes is replaced by a marker record rather
  /// than truncated.
  /// Never throws: a failure to build the record is reported on stderr and returned as
  /// kDropped.
  DiagnosticDestination Emit(const nlohmann::json& record) noexcept;

  /// Append an already-formatted line (ends in '\n'). Crash-safe.
  DiagnosticDestination AppendLine(const char* line, std::size_t length) noexcept;

  static constexpr std::size_t kMaxRecordBytes = 4096;
  static constexpr std::size_t kJournalCapacity = 8;

  // Test seams.
  const wchar_t* PrimaryDir() const { return primary_dir_; }
  const wchar_t* FallbackDir() const { return fallback_dir_; }
  const wchar_t* HostDir() const { return host_dir_; }
  bool PrimaryReady() const { return primary_ready_; }
  bool FallbackReady() const { return fallback_ready_; }
  bool HostDirReady() const { return host_dir_ready_; }
  std::size_t JournalFlushed() const { return flushed_upto_.load(); }
  std::size_t JournalSize() const { return journal_count_.load(); }

 private:
  struct JournalEntry {
    AttachStep step;
    AttachOutcome outcome;
    std::uint32_t detail;
    bool has_actual;
    unsigned char actual[kPreprocessPrologueSize];
  };

  void EmitJournalEntry(const JournalEntry& entry);
  DiagnosticDestination EmitChecked(const nlohmann::json& record);
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
};

}  // namespace nevr_host
