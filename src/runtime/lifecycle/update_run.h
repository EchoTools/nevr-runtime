#pragma once

// The runtime's half of PC self-update, the two jobs it does with files (docs/adr/0024-pc-self-update.md, sections 3
// and 6): the check (stage a release through the core, verify it, publish update\staged.json) and the confirmation
// (the swapped release has started: record it, clear update\pending.json). Both take every dependency as an argument
// (the core, the verifier, the log, the clock), so the tests run them against a fake core in a temp folder and
// update_worker.cpp runs them against the real one. No Log() here: every decision is one line through `LogFn`.
//
// Every path is composed here from the nevr root and the closed vocabulary (core/update/names.h);
// nothing is read from a file to name a path, and nothing is written under the game folder.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/update/fs.h"
#include "runtime/lifecycle/update_fs_win32.h"
#include "runtime/lifecycle/update_policy.h"

namespace nevr_update {

/// 0 debug, 1 info, 2 warning, 3 error. The line is `[NEVR.UPDATE] <event> key=value ...`, without a trailing newline.
using LogFn = std::function<void(int level, const std::string& line)>;
inline constexpr int kDebug = 0, kInfo = 1, kWarning = 2, kError = 3;

/// The game's LogLevel bit (abi/echovr.h: Debug 1, Info 2, Warning 4, Error 8) for a level above; an unknown level is Info.
/// update_process.cpp static_asserts these against the enum, so the mapping is tested here and cannot drift from it.
constexpr int EchoLevelBits(int level) noexcept {
  return level == kDebug ? 0x1 : level == kWarning ? 0x4 : level == kError ? 0x8 : 0x2;
}

/// The core's C ABI as the worker uses it (installer/include/nevr_install.h). update_core_ffi.cpp binds the real one; a
/// test supplies its own. Each function reports a failure through `error` (the core's NevrLastError text).
struct Core {
  std::function<bool(const std::string& config_json, std::string* error)> open;
  std::function<bool(std::string* error)> manifest_fetch;
  std::function<bool(const std::string& root_dir, const std::string& current_json, std::string* result_json,
                     std::string* error)>
      stage;
  /// `state` is one of installed, unchanged, restored, needs-user.
  std::function<void(const std::string& state, const std::string& reason)> run_end;
  /// NevrPcDiscard: the staged release was refused after the stage; the next stage does not fetch it again.
  std::function<bool(const std::string& root_dir, const std::string& version, const std::string& runtime_sha256,
                     const std::string& reason, std::string* error)>
      discard;
  std::function<void()> close;
};

struct VerifyOutcome {
  bool ok = false;
  /// The verifier ran and refused the file. False for a file that could not be opened or read: that may be a scanner's lock, and a
  /// release is discarded only for a verdict, never for a read.
  bool refused = false;
  std::string reason;  ///< the verifier's text (`Verdict::Describe`)
};

/// How long the runtime waits for update\lock before it gives the write up for this launch.
inline constexpr std::uint32_t kLockTimeoutMs = 5000;
inline constexpr std::uint32_t kLockStepMs = 100;

struct CheckEnv {
  std::wstring root;               ///< the nevr root (%LOCALAPPDATA%\nevr\echovr)
  std::string version;             ///< the running runtime's version, from its NEVR-BUILD literal
  std::string runtime_sha256;      ///< sha256 of the file this process loaded
  std::optional<std::string> config_channel;  ///< update.channel
  Core core;
  /// Authenticode on one staged file with the production policy (nevr_plugin_trust::ProductionPolicy, the denylist included).
  std::function<VerifyOutcome(const std::wstring& path)> verify;
  LogFn log;
  std::function<std::uint64_t()> now_ms;  ///< a monotonic clock
  /// The whole stage's budget (ADR 0024, section 3: 120 s). A result that arrives after it is not published.
  std::uint64_t budget_ms = 120000;
  std::uint32_t lock_timeout_ms = kLockTimeoutMs;
  /// Every file operation on update\ goes through this (core/update/fs.h). The PC edge by default.
  const UpdateFs* fs = &Win32UpdateFs();
};

enum class CheckAction { kNone, kStaged, kBlocked, kFailed };
const char* CheckActionWord(CheckAction action);  ///< none, staged, blocked, failed

struct CheckOutcome {
  CheckAction action = CheckAction::kFailed;
  std::string reason;
  std::string version;  ///< the release the channel named, when the core said
  std::string direction;
};

/// One check, start to end (ADR 0024, section 3). Writes `update check`, `update verify`, `update published`.
/// Never throws.
CheckOutcome RunCheck(const CheckEnv& env) noexcept;

enum class ConfirmResult {
  kConfirmed,
  kNotPending,            ///< no update\pending.json: nothing to confirm
  kNotThePendingRelease,  ///< pending.json names another runtime than this process loaded
  kPendingUnreadable,
  kStateUnreadable,
  kLockUnavailable,
  kWriteFailed,
};
const char* ConfirmResultWord(ConfirmResult result);

struct ConfirmEnv {
  std::wstring root;
  std::string runtime_sha256;  ///< sha256 of the file this process loaded
  const char* role = "client";  ///< client or server
  LogFn log;
  std::uint32_t lock_timeout_ms = kLockTimeoutMs;
  const UpdateFs* fs = &Win32UpdateFs();  ///< as CheckEnv::fs
};

/// ADR 0024, section 6, "The mark": under the lock, when update\pending.json's `to.sha256` equals the sha256 of the
/// file this process loaded, write update\state.json (`current` is the new release), remove pending.json and
/// `crashed`, and log `update confirmed`. A runtime that is not the pending one confirms nothing. `after_ms` is the
/// per-frame time the mark waited, for the line. Never throws.
ConfirmResult UpdateConfirm(const ConfirmEnv& env, std::uint64_t after_ms) noexcept;

}  // namespace nevr_update
