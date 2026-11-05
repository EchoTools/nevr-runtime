#pragma once

// The host's half of self-update, first part: the swap (docs/adr/0024-pc-self-update.md, section 4).
//
// Once per start, before RuntimeLoader::Load probes the runtime path, the host looks for update\ in the nevr root. When it
// does not exist the host does nothing and writes nothing. When it does:
//   1. the host takes update\lock (exclusive, delete on close), retrying for up to 30 s, and keeps it until the
//      runtime is mapped (RuntimeLoader::Load releases it before initialize);
//   2. an interrupted swap (update\swap.json) is recovered: phase out or in is undone to the last whole state, phase
//      done finishes its cleanup; a journal that does not parse stops the game (reason bad_journal), and so does one
//      that cannot be opened (journal_unreadable, with the Win32 error);
//   3. a staged release (update\staged.json) is checked again (confined names, size, sha256, the Authenticode
//      verifier with the host's policy); a file that fails is discarded and counted, the third discard quarantines;
//   4. it is swapped in only when no other process has the root's runtime mapped and no staged file would replace a
//      file self-update does not own: the journal is written, the owned files move to update\previous\<from>\, the
//      staged files move into the root, then update\state.json (owned), update\pending.json (boot_attempts 1) and
//      the cleanup.
// No folder the swap moves into, makes or deletes from is followed when it is a link (reason reparse_point, with the
// folder), and a file the host cannot open is never taken for a damaged one.
// Every outcome is one update_swap record in the run's main log (tag NEVR.HOST).
//
// The second part (#2105, ADR 0024 section 6) is a release that is pending, swapped in and not yet confirmed started
// (update\pending.json):
//   * at every start the host decides what to do with it. A release that failed (failed_reason), that crashed
//     (update\crashed) or that had two starts and no confirmation (boot_attempts 2) is rolled back before anything loads:
//     under a `rollback` journal each file of the pending release moves to update\failed\<to>\, each file of
//     update\previous\<from>\ comes back, and state.json quarantines the failed release. Otherwise the start is one more
//     try (boot_attempts + 1) and the pending release loads. A rollback and a count wait while another process has the
//     root's runtime mapped, so a second start inside the first one's trial cannot condemn the release;
//   * a staged release waits while one is pending (it would move an unconfirmed release over the last confirmed one);
//   * RollBackInProcess puts the previous release back in the same start for a pending runtime that never ran (refused,
//     absent, not loadable), and MarkPendingFailed records failed_reason for one that was mapped and failed, so the next
//     start rolls back;
//   * a rollback is never undone: an interrupted one is completed by the next start.
// The host tells the runtime it can do all this in NevrHostContext::capabilities (core/runtime_abi.h).
//
// No folder the swap moves into, makes or deletes from is followed when it is a link (reason reparse_point, with the
// folder), and a file the host cannot open is never taken for a damaged one.
// Every outcome is one update_swap or update_rollback record in the run's main log (tag NEVR.HOST).

#include "runtime/host/bootstrap_diagnostics.h"

#include <windows.h>

#include <cstdint>

namespace nevr_host {

/// update\lock: one holder at a time across every process that shares the nevr root (ADR 0024, section 4.1).
class UpdateLock {
 public:
  UpdateLock() = default;
  UpdateLock(const UpdateLock&) = delete;
  UpdateLock& operator=(const UpdateLock&) = delete;
  ~UpdateLock();

  /// Open `<update_dir>\lock` with share mode 0 and delete-on-close, trying every `step_ms` for up to `timeout_ms`.
  bool Acquire(const wchar_t* update_dir, DWORD timeout_ms, DWORD step_ms) noexcept;
  void Release() noexcept;
  bool Held() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

/// How long a start waits for update\lock, and how often it tries (ADR 0024, section 4.1).
inline constexpr DWORD kUpdateLockTimeoutMs = 30000;
inline constexpr DWORD kUpdateLockStepMs = 100;
/// Discards of one release after which it is quarantined (ADR 0024, section 4.3).
inline constexpr std::uint32_t kDiscardsToQuarantine = 3;

enum class UpdateAction {
  kNoUpdateFolder,  ///< no update\: nothing was read or written
  kLoad,            ///< load what is in the root now (the lock is held when it was taken)
  kStop,            ///< the root may be half moved: the game must not load anything (reason says why)
};

struct UpdateResult {
  UpdateAction action = UpdateAction::kNoUpdateFolder;
  /// For kStop: lock_timeout, bad_journal, journal_unreadable, reparse_point, move_failed, internal_error, no_previous,
  /// previous_unverified or runtime_not_writable.
  const char* reason = nullptr;
  /// For kLoad: the release in the root is pending (just swapped in, or still on its trial). A load of it that fails is
  /// the host's to handle (RollBackInProcess, MarkPendingFailed).
  bool pending = false;
};

/// The step ADR 0024 section 4 runs at every start. `runtime_path` is the path RuntimeLoader::Load will probe
/// (`<root>\nevr-runtime.dll`); its folder is the root. Writes its records through `diagnostics`. Never throws.
UpdateResult RunUpdateAtStart(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics,
                                   UpdateLock& lock) noexcept;

/// R1 (ADR 0024, section 6): the pending release's runtime never ran (refused, absent, not loadable), so the previous
/// release is put back and the same start loads it. Takes the lock again (the load released it) and keeps it on kLoad,
/// for the load that follows to release once the runtime is mapped. `reason` is the load outcome's name. kStop: nothing
/// that can be verified is left to load (no_previous, previous_unverified) or the root could not be put back whole;
/// the failed release is then marked, so the next start tries again. Never throws.
UpdateResult RollBackInProcess(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock,
                               const char* reason) noexcept;

/// R1 for a runtime that was mapped and then failed (missing entry, null descriptor, ABI rejected, initialize failed):
/// it stays mapped (production never frees it), so the rollback is the next start's. Writes `failed_reason` into
/// update\pending.json under the lock. True when it was written. Never throws.
bool MarkPendingFailed(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock,
                       const char* reason) noexcept;

#if defined(NEVR_HOST_TESTING)
/// Test seam: the lock wait (production: kUpdateLockTimeoutMs, kUpdateLockStepMs). Zero restores the defaults.
void SetUpdateLockTimingForTest(DWORD timeout_ms, DWORD step_ms) noexcept;
/// Test seam: called after each step of a swap or a recovery that changes the disk (a journal write, a move, a file
/// removed), with its 1-based number. When it returns true the start ends right there, as a kill would end it.
void SetUpdateStepHookForTest(bool (*hook)(int step)) noexcept;
/// Test seam: the `move_number`-th file move of a start fails with `error` (0: none), as a move to another volume does.
void SetUpdateMoveFailureForTest(int move_number, DWORD error) noexcept;
#endif

}  // namespace nevr_host
