#pragma once

// Host-owned bounded bootstrap/crash diagnostic recorder for the stable
// BugSplat64.dll host (docs/design/2026-10-05-bugsplat-nevr-split.md step 3).
//
// One bounded JSON line is appended per event to
//   %LOCALAPPDATA%\EchoVR\logs\nevr-bootstrap.jsonl
// with fallback to
//   %TEMP%\EchoVR\logs\nevr-bootstrap.jsonl
// when the primary is missing or unwritable.
//
// Constraints (design "Early bootstrap without doing the full runtime under the
// loader lock"): the PATH state lives in fixed wchar_t[MAX_PATH] members (no
// heap-owned path state); the record is built with nlohmann::json so escaping is
// correct by construction (AGENTS.md "no hand-built serialization"); it is
// written with a single bounded WriteFile append. Prepare() performs no I/O and
// no directory creation, so it is safe under the loader lock. Emit() is meant to
// run after attach returns (deferred reporting), where directory creation and
// heap are allowed. Emit() never recurses and never calls the NEVR logger.

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <nlohmann/json_fwd.hpp>

namespace NevrHost {

enum class DiagnosticDestination {
  kPrimary,   // %LOCALAPPDATA%\EchoVR\logs
  kFallback,  // %TEMP%\EchoVR\logs
  kDropped,   // neither writable, or the record was rejected
};

/// Why the host's DLL_PROCESS_ATTACH could not install the PreprocessCommandLine
/// rendezvous. Recorded in fixed memory during DllMain (no heap, no I/O); the
/// JSON diagnostic is built and written later, by FlushPendingAttachDiagnostic.
/// Delivery is best-effort: the flush runs only from host entry points that fire
/// without the rendezvous detour (the crash-reporter constructor and
/// NEVR_SetGameModule), so in direct-import mode a failed detour leaves no
/// guaranteed-early trigger — the record flushes when the game constructs its
/// crash reporter, or not until then.
enum class AttachFailure : std::uint32_t {
  kNone = 0,
  kImageRejected = 1,     // the game module is not a supported echovr.exe build
  kPrologueMismatch = 2,  // the pinned PreprocessCommandLine prologue moved
  kHookFailed = 3,        // MinHook initialize/create/enable failed
};

class BootstrapDiagnostics {
 public:
  // Copy %LOCALAPPDATA% and %TEMP% into fixed buffers and precompute the two log
  // directories. Bounded GetEnvironmentVariableW only: no allocation, no I/O, no
  // directory creation. Safe under the loader lock.
  void Prepare();

  // Append one bounded JSON record. The target directory is created on demand. A
  // record whose serialized form exceeds kMaxRecordBytes is refused rather than
  // truncated: a truncated JSON line is unparseable and worse than no line.
  DiagnosticDestination Emit(const nlohmann::json& record);

  // Record an attach failure in fixed memory. No heap, no I/O, no locks: safe
  // under the loader lock. detail is an optional win32 / MH status / prologue
  // byte for the diagnostic.
  void RecordAttachFailure(AttachFailure failure, std::uint32_t detail = 0);

  // Build and emit the attach-failure diagnostic once. Idempotent (a second call
  // is a no-op). Called after attach returns from the two host entry points that
  // fire without the rendezvous detour — the crash-reporter constructor and
  // NEVR_SetGameModule — so delivery is best-effort, not guaranteed-early.
  void FlushPendingAttachDiagnostic();

  static constexpr std::size_t kMaxRecordBytes = 4096;

  // Test seams.
  const wchar_t* PrimaryDir() const { return primary_dir_; }
  const wchar_t* FallbackDir() const { return fallback_dir_; }
  bool PrimaryReady() const { return primary_ready_; }
  bool FallbackReady() const { return fallback_ready_; }
  AttachFailure AttachFailureKind() const { return attach_failure_; }
  bool AttachDiagnosticFlushed() const { return attach_flushed_.load(); }

 private:
  bool EmitTo(const wchar_t* dir, const char* line, std::size_t length);

  wchar_t local_app_data_[MAX_PATH] = {};
  wchar_t temp_[MAX_PATH] = {};
  wchar_t primary_dir_[MAX_PATH] = {};   // %LOCALAPPDATA%\EchoVR\logs
  wchar_t fallback_dir_[MAX_PATH] = {};  // %TEMP%\EchoVR\logs
  bool primary_ready_ = false;
  bool fallback_ready_ = false;
  AttachFailure attach_failure_ = AttachFailure::kNone;
  std::uint32_t attach_detail_ = 0;
  std::atomic_bool attach_flushed_{false};
};

}  // namespace NevrHost
