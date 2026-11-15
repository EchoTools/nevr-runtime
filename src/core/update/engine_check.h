#pragma once

// The runtime's two jobs with files in the self-update (docs/adr/0024-pc-self-update.md, sections 3 and 6), platform-free: the
// check (stage a release through the core, verify it, publish update\staged.json) and the confirmation (the swapped release has
// started: record it, clear update\pending.json). Every file operation on update\ goes through UpdateFs (core/update/fs.h), every
// path is UTF-8 and composed here from the nevr root and the closed vocabulary (core/update/names.h), and every dependency is an
// argument: the core, the verifier, the log, the clock and the fs. The platform edge (runtime/lifecycle/update_run.h on PC)
// converts the wide root and the wide verifier path. The tests run these functions against a fake core in a temp folder.
// No Log() here: every decision is one line through `LogFn`.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/update/fs.h"

namespace nevr_update {

/// 0 debug, 1 info, 2 warning, 3 error. The line is `[NEVR.UPDATE] <event> key=value ...`, without a trailing newline.
using LogFn = std::function<void(int level, const std::string& line)>;
inline constexpr int kDebug = 0, kInfo = 1, kWarning = 2, kError = 3;

/// The core's C ABI as the engine uses it (installer/include/nevr_install.h). The platform edge binds the real one; a test supplies its
/// own. Each function reports a failure through `error` (the core's NevrLastError text).
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

/// What the stage result says one staged file is. The core checked the bytes against the signed manifest when it staged them; a
/// verifier that wants the bytes checked again compares them with this.
struct ExpectedFile {
  std::uint64_t size = 0;
  std::string sha256;  ///< 64 lowercase hex
};

struct VerifyOutcome {
  bool ok = false;
  /// The verifier ran and refused the file. False for a file that could not be opened or read: that may be a scanner's lock, and a
  /// release is discarded only for a verdict, never for a read.
  bool refused = false;
  std::string reason;  ///< the verifier's text (`Verdict::Describe`)
};

/// The lock's timeout and step are fs.h's kLockTimeoutMs and kLockStepMs (one definition, the UpdateFs interface's).
/// The whole stage's budget (ADR 0024, section 3: 120 s). A result that arrives after it is not published.
inline constexpr std::uint64_t kCheckBudgetMs = 120000;

struct CheckEnv {
  std::string root;  ///< the nevr root, UTF-8 (%LOCALAPPDATA%\nevr\echovr on PC); empty when there is none
  /// The platform's root text is not UTF-8 text: nothing is read from the root and the check ends `root_unconvertible`.
  bool root_unconvertible = false;
  std::string platform;  ///< the core's platform word, "pc" on PC (ADR 0024, section 3); required
  std::string version;             ///< the running runtime's version, from its NEVR-BUILD literal
  std::string runtime_sha256;      ///< sha256 of the file this process loaded
  std::optional<std::string> config_channel;  ///< update.channel
  Core core;
  /// The verifier of one staged file, by its UTF-8 path and the entry the stage result gave it. The platform edge decides what
  /// verifies it: PC checks the Authenticode signature and ignores the entry; Quest checks the bytes against it.
  std::function<VerifyOutcome(const std::string& path, const ExpectedFile& expected)> verify;
  LogFn log;
  std::function<std::uint64_t()> now_ms;  ///< a monotonic clock
  std::uint64_t budget_ms = kCheckBudgetMs;
  std::uint32_t lock_timeout_ms = kLockTimeoutMs;
  /// Every file operation on update\ goes through this (core/update/fs.h). Required.
  const UpdateFs* fs = nullptr;
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
  std::string root;  ///< UTF-8, as CheckEnv::root
  bool root_unconvertible = false;  ///< as CheckEnv::root_unconvertible
  std::string runtime_sha256;  ///< sha256 of the file this process loaded
  const char* role = "client";  ///< client or server
  LogFn log;
  std::uint32_t lock_timeout_ms = kLockTimeoutMs;
  const UpdateFs* fs = nullptr;  ///< as CheckEnv::fs; required
};

/// ADR 0024, section 6, "The mark": under the lock, when update\pending.json's `to.sha256` equals the sha256 of the
/// file this process loaded, write update\state.json (`current` is the new release), remove pending.json and
/// `crashed`, and log `update confirmed`. A runtime that is not the pending one confirms nothing. `after_ms` is the
/// per-frame time the mark waited, for the line. Never throws.
ConfirmResult UpdateConfirm(const ConfirmEnv& env, std::uint64_t after_ms) noexcept;

}  // namespace nevr_update
