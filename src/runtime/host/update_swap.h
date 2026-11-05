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
// Not here (ADR 0024 step 2, #2105): what happens to a pending release (rollback, boot_attempts, the crash marker)
// and the host context's capability word. Until they exist this host does not tell a runtime it can swap.

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
  /// For kStop: lock_timeout, bad_journal, journal_unreadable, reparse_point, move_failed or internal_error.
  const char* reason = nullptr;
};

/// The step ADR 0024 section 4 runs at every start. `runtime_path` is the path RuntimeLoader::Load will probe
/// (`<root>\nevr-runtime.dll`); its folder is the root. Writes its records through `diagnostics`. Never throws.
UpdateResult RunUpdateAtStart(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics,
                                   UpdateLock& lock) noexcept;

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
