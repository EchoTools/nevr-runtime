#pragma once

// The decision a start makes about a pending release (docs/adr/0024-pc-self-update.md, section 6, R2), over the facts the
// start reads through UpdateFs (fs.h). Platform-free: no Windows header and no I/O of its own. The facts the file
// system cannot give are the edge's and are passed in: the digest of the root's runtime (UpdateFs reads files of at most
// nevr_update_names::kMaxJsonBytes, and the runtime is larger), the answer to the in-use question, and whether this
// start finished a swap's cleanup. The decision comes back as data. The edge carries it out: the removals, the count, the
// rollback and the records, so every effect stays where it is until the journal moves into the engine.

#include "core/update/fs.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace nevr_update {

/// Starts without the mark after which a pending release that has no recorded failure and no crash is rolled back at the
/// next start (ADR 0024, section 6, R2). A new release gets one start fewer than this to reach the mark.
inline constexpr std::uint64_t kBootAttemptsToRollBack = 2;

/// The length of a SHA-256 digest written as lower-case hex.
inline constexpr std::size_t kSha256HexChars = 64;

/// True for exactly kSha256HexChars characters of [0-9a-f].
bool IsSha256Hex(std::string_view text) noexcept;

/// A reason word: lower case, digits and '_', at most 40 characters.
bool IsReasonToken(std::string_view text) noexcept;

/// The JSON of a file of update\: at most nevr_update_names::kMaxJsonBytes, nested at most nevr_update_names::kMaxJsonDepth
/// levels deep (never parsed into a tree when deeper: copying it recurses once per level), and parsed. False when it is
/// damage.
bool ParseJsonText(std::string_view text, nlohmann::json* out);

/// pending.json (ADR 0024, section 2): {"to": {"version", "sha256"}, "from": {"version", "sha256"} or null,
/// "boot_attempts", "failed_reason": null or a reason word}. `has_from` is false when the swap found no previous runtime.
struct Pending {
  std::string to_version;
  std::string to_sha256;
  bool has_from = false;
  std::string from_version;
  std::string from_sha256;
  std::uint64_t boot_attempts = 0;
  std::string failed_reason;  ///< empty: null
};

/// True when `doc` is a pending record this host writes; `out` is set only then.
bool ParsePending(const nlohmann::json& doc, Pending* out);

/// The answer the edge gives to the in-use question: may the root's runtime be written (is no other process running it)?
struct WriteAccess {
  enum class Kind {
    kWritable,     ///< nothing maps the runtime; a rollback or a swap may move it
    kInUse,        ///< another process maps it (a sharing violation on Windows): the start is not a try and condemns nothing
    kNotWritable,  ///< it cannot be written for another cause: nothing is moved
  };
  Kind kind = Kind::kWritable;
  /// kNotWritable: the platform's error. 0 when the platform gave none.
  std::uint32_t os_error = 0;
};

/// The facts about the pending record and the marker the decision reads.
struct PendingFacts {
  /// update\pending.json, read through UpdateFs.
  ReadResult pending_read = ReadResult::kAbsent;
  /// Its bytes when pending_read is kOk.
  std::string pending_text;
  /// The platform's error for a kUnreadable read: UpdateFs::LastOsError, taken by ReadPendingFacts straight after the read. 0 when
  /// the read did not fail.
  std::uint32_t pending_os_error = 0;
  /// update\crashed exists, read through UpdateFs.
  bool crashed = false;
  /// The sha256 of the runtime in the root, supplied by the edge; empty when the root holds no readable runtime.
  std::string root_runtime_sha256;
  /// This start finished a swap's cleanup, so that swap's start never reached its load (the load of this start is its first try).
  bool finished_swap = false;
};

/// Reads update\pending.json and update\crashed through `fs`; `update_dir` is the update folder as `fs` spells paths.
/// `root_runtime_sha256` and `finished_swap` are the edge's and are copied in unchanged.
PendingFacts ReadPendingFacts(const UpdateFs& fs, const std::string& update_dir, std::string root_runtime_sha256,
                              bool finished_swap);

enum class PendingAction {
  kNone,                    ///< no pending record: nothing is pending
  kUnreadable,              ///< the record is there and cannot be read: nothing is judged, counted or swapped (pending_unreadable)
  kDamaged,                 ///< a record this host did not write: the edge removes it and update\crashed (bad_pending)
  kStale,                   ///< the root holds another runtime: the edge removes the record and the marker (pending_stale)
  kRollBack,                ///< the release is rolled back for `reason`
  kDeferredInUse,           ///< a rollback is due for `reason`, but another process maps the runtime: nothing is condemned
  kBlockedNotWritable,      ///< a rollback is due for `reason`, but the runtime cannot be written: nothing is moved
  kCountTry,                ///< one more try: the edge writes `counted`
  kNotCountedInUse,         ///< another process maps the runtime: a start beside a running one is not a try
  kNotCountedNotWritable,   ///< the runtime cannot be written: not a try
  kNotCountedSwapFinished,  ///< this start finished a swap's cleanup: that swap's start never loaded, so this is not a second try
};

struct PendingDecision {
  PendingAction action = PendingAction::kNone;
  /// The parsed record, for every action that names it (all but kNone, kUnreadable and kDamaged).
  Pending pending;
  /// kRollBack: the rollback reason. kDeferredInUse and kBlockedNotWritable: the cause (the same word).
  std::string reason;
  /// kUnreadable: the read error. kBlockedNotWritable and kNotCountedNotWritable: the runtime's error.
  std::uint32_t os_error = 0;
  /// kCountTry: the record with boot_attempts counted up by one, to be written as pending.json.
  nlohmann::json counted;
};

/// ADR 0024, section 6, R2: the decision for the facts. `write_access` is asked at most once, and only after the record is
/// judged not stale (so a record that is dropped or held never asks).
PendingDecision DecidePending(const PendingFacts& facts, const std::function<WriteAccess()>& write_access);

}  // namespace nevr_update
