#pragma once

// The start of the self-update engine (docs/adr/0024-pc-self-update.md, sections 4 and 6): the journal and its recovery, the
// pending decision, the swap and the rollback, written over the platform edges of edges.h and fs.h. The same code runs on
// PC (runtime/host/update_swap.cpp binds it to Win32) and, once the Quest edges are written, on Quest. It includes no platform
// header and holds no global state: everything it needs arrives in a StartContext.
//
// Every path the engine uses is composed from the nevr root and the closed vocabulary of names.h, with '/' inside the
// relative part; the edge turns it into its own separator (UpdateFs::Separator).

#include "core/update/edges.h"
#include "core/update/fs.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace nevr_update {

/// Discards of one release after which it is quarantined (ADR 0024, section 4.3). The one definition: the host re-exports it.
inline constexpr std::uint32_t kDiscardsToQuarantine = 3;

/// What the host does after a start (the host's UpdateAction, runtime/host/update_swap.h).
enum class StartAction : std::uint8_t {
  kNoUpdateFolder,  ///< no update\: nothing was read or written
  kLoad,            ///< load what is in the root now
  kStop,            ///< the root may be half moved: nothing loads (`reason` says why)
};

struct StartResult {
  StartAction action = StartAction::kNoUpdateFolder;
  /// For kStop: lock_timeout, bad_journal, journal_unreadable, reparse_point, move_failed, internal_error, no_previous,
  /// previous_unverified, runtime_in_use, runtime_not_writable, rollback_blocked, state_unreadable or pending_unreadable.
  const char* reason = nullptr;
  /// For kLoad: the release in the root is pending (just swapped in, or on its trial).
  bool pending = false;
};

/// The outcome of a load, in the engine's three classes (ADR 0024, section 6). kNotLoaded is a stop that is not the
/// release's doing, or a start with no path.
enum class LoadClass : std::uint8_t { kLoaded, kNotLoaded, kNeverRan, kEntryOnly, kInitFailed };

/// What a start does with a pending release whose load failed.
enum class PendingFailure : std::uint8_t {
  kNone,      ///< loaded, or a stop that is not the release's doing
  kNeverRan,  ///< no line of the runtime ran: the previous release is put back in the same start
  kMapped,    ///< the runtime was mapped and then failed: it stays mapped, so the next start puts the previous one back
};

/// R1's classification. `code_before_entry` is RuntimeLoaderEdge::CodeBeforeEntry(). PC answers true, and so an entry-only
/// failure is kMapped, as it has always been. The false branch (a runtime with no constructors, which Quest would roll back
/// in the same start) is the Quest edges' decision, not written in this tree (ADR 0023, section 7): until it is, both values answer kMapped, the
/// stop that PC takes, never a load of a runtime nothing verified.
PendingFailure ClassifyPendingFailure(LoadClass load, bool code_before_entry) noexcept;

/// The two hooks a start consults, as function pointers so the engine holds no state of its own. Production passes nulls: no
/// step ends a start and no move fails.
struct StartHooks {
  /// True when the start must end after its `step`-th change to the disk, as a kill would end it.
  bool (*step_ends)(int step) = nullptr;
  /// True, with `*os_error` set, when the `move`-th file move of a start must fail.
  bool (*move_fails)(int move, int* os_error) = nullptr;
};

/// How long a start waits for update\lock, and how often it tries (ADR 0024, section 4.1).
struct LockTiming {
  std::uint32_t timeout_ms = 0;
  std::uint32_t step_ms = 0;
};

/// The edges one start uses. Every reference outlives the start.
struct StartEdges {
  const UpdateFs& fs;
  const ArtifactVerifier& verifier;
  const InUseCheck& in_use;
  const RollbackTarget& rollback;
  const RuntimeLoaderEdge& loader;
  UpdateLog& log;
  const UpdateClock& clock;
};

struct StartContext {
  /// The nevr root (the folder the runtime sits in), in the edge's path form, without a trailing separator. Empty means no
  /// root: the start does nothing.
  std::string root;
  StartEdges edges;
  StartHooks hooks;
  LockTiming lock_timing;
  /// update\lock is taken into this slot and stays there when a start leaves it held (the load releases it). The slot is
  /// the host's; a start that finds it held does not take it again.
  std::unique_ptr<FsLock>* lock = nullptr;
};

/// The step ADR 0024 section 4 runs at every start. Never throws: an exception inside it is recorded and answered as
/// the start's own (`internal_error`).
StartResult RunStart(const StartContext& context) noexcept;

/// R1, never ran (ADR 0024, section 6): the previous release is put back now, in this start. Takes the lock if it is not
/// held. Never throws.
StartResult RollBackNowIn(const StartContext& context, const char* reason) noexcept;

/// R1, mapped (ADR 0024, section 6): writes `failed_reason` into update\pending.json under the lock. True when it is
/// there afterwards. Never throws.
bool MarkFailedIn(const StartContext& context, const char* reason) noexcept;

// ---- The pending decision (ADR 0024, section 6, R2): pure, over the facts the edge reads ----

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
