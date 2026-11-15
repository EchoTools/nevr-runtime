// The Quest's update start (update_start.h). The engine is core/update's, the same code PC runs; this file is the
// Quest's edges (UpdateFs is the engine's POSIX edge, fs_posix.cpp) and the orchestration of the load that follows the
// decision, which mirrors PC's RuntimeLoader::LoadAtStart.

#include "quest/shim/update_start.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "core/build_literal_scan.h"
#include "core/update/engine_start.h"
#include "core/update/fs.h"
#include "core/update/names.h"
#include "quest/data/sha256.h"

namespace nevr_quest::update_start {
namespace {

namespace engine = nevr_update;
namespace names = nevr_update_names;
namespace rl = runtime_load;
using json = nlohmann::json;

inline constexpr char kRecordTag[] = "NEVR.HOST";
inline constexpr char kEventGameStop[] = "game_stop";
inline constexpr char kStopReason[] = "update_failed";
inline constexpr char kConsequencePending[] = "the next start puts the previous release back";
inline constexpr char kConsequenceConfirmed[] = "the next start runs this release again";
inline constexpr char kOutcomeField[] = "outcome";
inline constexpr char kLevelField[] = "level";
inline constexpr char kEventField[] = "event";
inline constexpr char kLevelDebug[] = "debug";
inline constexpr char kLevelInfo[] = "info";
inline constexpr char kLevelWarn[] = "warn";
inline constexpr char kLevelError[] = "error";
inline constexpr char kOutcomeBlocked[] = "blocked";
inline constexpr char kOutcomeFailed[] = "failed";
inline constexpr char kOutcomeRolledBack[] = "rolled_back";
inline constexpr char kOutcomeScheduled[] = "scheduled";
inline constexpr char kIsoFormat[] = "%Y-%m-%dT%H:%M:%SZ";
inline constexpr std::size_t kIsoCapacity = 32;
inline constexpr std::size_t kPathCapacity = 4096;
inline constexpr std::size_t kReadChunkBytes = 64 * 1024;
inline constexpr std::uint64_t kMsPerSecond = 1000;
inline constexpr std::uint64_t kNsPerMs = 1000000;
inline constexpr char kPathSeparator[] = "/";
// The Quest runtime has no nEVR constructors (its static-init check, the Quest build's justfile), so nothing of it runs
// before its entry: a failure at the entry is a load that never ran (ClassifyPendingFailure, engine_start.h).
inline constexpr bool kQuestCodeBeforeEntry = false;

// The update root the runtime is handed (HostInputs::update_root). The context keeps the pointer for the life of the
// process, so the text lives here: zero-initialised, with no constructor, because the shim has no load-time
// initializer.
char g_updateRoot[kPathCapacity];

// Closes a descriptor when the scope ends.
struct FdGuard {
  int fd;
  ~FdGuard() {
    if (fd >= 0) ::close(fd);
  }
};

// The engine's class of a load outcome (edges.h). A file that cannot be opened or mapped ran nothing: kNeverRan. A host input that
// is missing (no shim handle, no libr15 base, no build ID) is not the release's doing: kNotLoaded.
engine::LoadClass ClassOf(rl::Outcome outcome) noexcept {
  switch (outcome) {
    case rl::Outcome::kLoaded:
      return engine::LoadClass::kLoaded;
    case rl::Outcome::kAbsent:
    case rl::Outcome::kLoadFailed:
      return engine::LoadClass::kNeverRan;
    case rl::Outcome::kHostContextRefused:
      // The host did not start the runtime (a required input is absent): this is not the release's doing, so it is a stop that
      // judges nothing (engine_start.h, LoadClass::kNotLoaded). StartChecked returns it before any rollback.
      return engine::LoadClass::kNotLoaded;
    case rl::Outcome::kMissingEntry:
    case rl::Outcome::kNullDescriptor:
    case rl::Outcome::kAbiRejected:
      return engine::LoadClass::kEntryOnly;
    case rl::Outcome::kInitFailed:
      return engine::LoadClass::kInitFailed;
  }
  return engine::LoadClass::kNotLoaded;
}

// The level a record carries (DESIGN section 8): failed is Error; blocked, rolled_back and scheduled are Warning; the
// rest Info. A record that names its own level keeps it.
std::uint32_t LevelOf(const json& record) {
  if (record.contains(kLevelField) && record[kLevelField].is_string()) {
    const std::string level = record[kLevelField].get<std::string>();
    if (level == kLevelDebug) return rl::kLogLevelDebug;
    if (level == kLevelWarn) return rl::kLogLevelWarning;
    if (level == kLevelError) return rl::kLogLevelError;
    return rl::kLogLevelInfo;
  }
  std::string outcome;
  if (record.contains(kOutcomeField) && record[kOutcomeField].is_string())
    outcome = record[kOutcomeField].get<std::string>();
  if (outcome == kOutcomeFailed) return rl::kLogLevelError;
  if (outcome == kOutcomeBlocked || outcome == kOutcomeRolledBack || outcome == kOutcomeScheduled)
    return rl::kLogLevelWarning;
  return rl::kLogLevelInfo;
}

// A record's value as the sink's text: a string as it is, a boolean as true or false, null as empty, a number or a
// nested value as its JSON text.
std::string TextOf(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_null()) return std::string();
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  return value.dump();
}

// The digest of the file behind `fd`, read from its start with pread (the descriptor's offset is not used). Empty when
// a read fails; `*read_bytes` is what was read.
std::string HashFd(int fd, std::uint64_t* read_bytes) {
  data::Sha256 hash;
  std::vector<unsigned char> chunk(kReadChunkBytes);
  std::uint64_t offset = 0;
  for (;;) {
    const ssize_t n = ::pread(fd, chunk.data(), chunk.size(), static_cast<off_t>(offset));
    if (n < 0) {
      if (errno == EINTR) continue;
      *read_bytes = offset;
      return std::string();
    }
    if (n == 0) break;
    hash.Update(chunk.data(), static_cast<std::size_t>(n));
    offset += static_cast<std::uint64_t>(n);
  }
  *read_bytes = offset;
  return hash.Hex();
}

// The Quest's ArtifactVerifier for 6.18: a staged file's size and sha256 against its staged.json entry. There is no
// signature in 6.18 (the proof pair and NevrProofVerify are the 6.19 verifier), so a file asked for without an entry is
// checked only for being a regular file. KNOWN-WRONG: a runtime in the root is trusted by its place under update\ until
// the proof verifier lands; its tracking issue is not filed yet (the report of piece 15 holds the draft).
class QuestVerifier final : public engine::ArtifactVerifier {
 public:
  engine::ArtifactCheck Verify(const std::string& path, const engine::ArtifactExpect& expect) const override {
    engine::ArtifactCheck check;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      check.reason = "unreadable";
      return check;
    }
    const FdGuard guard = {fd};
    struct stat st = {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      check.reason = "unreadable";
      return check;
    }
    if (expect.check_digest) {
      if (static_cast<std::uint64_t>(st.st_size) != expect.size) {
        check.reason = "size";
        return check;
      }
      std::uint64_t read = 0;
      const std::string digest = HashFd(fd, &read);
      if (digest.empty() || read != expect.size) {
        check.reason = "unreadable";
        return check;
      }
      if (digest != expect.sha256) {
        check.reason = "sha256";
        return check;
      }
    }
    check.ok = true;
    return check;
  }

  engine::ArtifactFacts Measure(const std::string& path) const override {
    engine::ArtifactFacts facts;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      // Nothing at the path (or a file where a folder should be) is absent; any other failure is a file that cannot be
      // read.
      facts.unreadable = errno != ENOENT && errno != ENOTDIR;
      return facts;
    }
    const FdGuard guard = {fd};
    struct stat st = {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      facts.present = true;
      facts.unreadable = true;
      return facts;
    }
    facts.present = true;
    std::uint64_t read = 0;
    facts.sha256 = HashFd(fd, &read);
    if (facts.sha256.empty() || read != static_cast<std::uint64_t>(st.st_size)) {
      facts.sha256.clear();
      facts.unreadable = true;
      return facts;
    }
    facts.version = nevr_build_literal::VersionOfFile(fd, static_cast<std::uint64_t>(st.st_size));
    return facts;
  }
};

// The Quest has one app process per package, so nothing else maps the root's runtime: the answer is always free. The
// lock covers the rest (ADR 0024, section 4.4, as the design's InUseProbe).
class QuestInUse final : public engine::InUseCheck {
 public:
  engine::InUseAnswer Query(const std::string& runtime_path) const override {
    static_cast<void>(runtime_path);
    return engine::InUseAnswer{};
  }
};

// The previous release's runtime is the rollback target when it is in place and verifies. Otherwise it is absent, and
// the engine stops (no_previous): the start then loads the APK's baseline, because the Quest never stops for lack of a
// runtime.
class QuestRollback final : public engine::RollbackTarget {
 public:
  explicit QuestRollback(const engine::ArtifactVerifier& verifier) noexcept : verifier_(verifier) {}

  engine::RollbackCheck Check(const std::string& runtime_path) const override {
    engine::RollbackCheck check;
    struct stat st = {};
    if (::lstat(runtime_path.c_str(), &st) != 0) {
      check.state = errno == ENOENT ? engine::RollbackState::kAbsent : engine::RollbackState::kUnverified;
      if (check.state == engine::RollbackState::kUnverified) check.reason = "unreadable";
      return check;
    }
    const engine::ArtifactCheck verdict = verifier_.Verify(runtime_path, engine::ArtifactExpect{});
    if (!verdict.ok) {
      check.state = engine::RollbackState::kUnverified;
      check.reason = verdict.reason;
      return check;
    }
    check.state = engine::RollbackState::kUsable;
    return check;
  }

 private:
  const engine::ArtifactVerifier& verifier_;
};

// The Quest runtime has no nEVR constructors, so its load runs no runtime code before the entry (kQuestCodeBeforeEntry).
class QuestLoaderEdge final : public engine::RuntimeLoaderEdge {
 public:
  bool CodeBeforeEntry() const noexcept override { return kQuestCodeBeforeEntry; }
};

// The engine's records through the shim's sink: one sentinel record per decision, tagged NEVR.HOST, with the event and
// the fields the engine gives. The sink copies every string it is given, so the strings here need to live only for the
// call.
class QuestLog final : public engine::UpdateLog {
 public:
  explicit QuestLog(NevrQuestLogFn sink) noexcept : sink_(sink) {}

  void Emit(const json& record) override {
    if (sink_ == nullptr || !record.is_object()) return;
    try {
      const std::uint32_t level = LevelOf(record);
      std::string event;
      if (record.contains(kEventField)) event = TextOf(record[kEventField]);
      std::vector<std::pair<std::string, std::string>> pairs;
      for (const auto& [key, value] : record.items()) {
        if (key == kEventField || key == kLevelField) continue;
        pairs.emplace_back(key, TextOf(value));
      }
      std::vector<NevrQuestLogField> fields;
      fields.reserve(pairs.size());
      for (const auto& [key, value] : pairs) fields.push_back(NevrQuestLogField{key.c_str(), value.c_str()});
      sink_(level, kRecordTag, event.c_str(), nullptr, fields.data(), static_cast<std::uint32_t>(fields.size()));
    } catch (const std::exception&) {
      // A record that cannot be built is dropped: the start decides the same either way.
    }
  }

 private:
  NevrQuestLogFn sink_;
};

// The monotonic clock for elapsed times and the UTC time for quarantine entries.
class QuestClock final : public engine::UpdateClock {
 public:
  std::uint64_t TickMs() const noexcept override {
    timespec now = {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * kMsPerSecond + static_cast<std::uint64_t>(now.tv_nsec) / kNsPerMs;
  }

  std::string UtcIso() const override {
    const std::time_t now = std::time(nullptr);
    std::tm utc = {};
    if (::gmtime_r(&now, &utc) == nullptr) return std::string();
    char text[kIsoCapacity] = {};
    if (std::strftime(text, sizeof text, kIsoFormat, &utc) == 0) return std::string();
    return std::string(text);
  }
};

// Releases update\lock. The load calls this once the runtime is mapped and before its initialize
// (HostInputs::before_initialize).
void ReleaseLock(void* arg) noexcept {
  auto* lock = static_cast<std::unique_ptr<engine::FsLock>*>(arg);
  if (lock != nullptr) lock->reset();
}

// The Result of a load of the update root's runtime that succeeded.
Result Updated(const std::string& path) {
  Result result;
  result.outcome = rl::Outcome::kLoaded;
  result.origin = Origin::kUpdate;
  result.path = path;
  return result;
}

// The baseline start: the lock is let go first, so the baseline's own load does not hold it.
Result Baseline(const Inputs& in, const rl::HostInputs& host, std::unique_ptr<engine::FsLock>* lock) {
  lock->reset();
  Result result;
  result.outcome = in.baseline_dir.empty() ? rl::StartBaselineBesideShim(in.in_shim, host)
                                           : rl::LoadBaseline(in.baseline_dir.c_str(), host);
  result.origin = Origin::kBaseline;
  return result;
}

// game_stop, once, for a release whose initialize failed (the process ends right after the caller returns).
void WriteStop(NevrQuestLogFn sink, rl::Outcome outcome, bool pending) {
  if (sink == nullptr) return;
  const std::string code = std::to_string(kUpdateFailedExitCode);
  const NevrQuestLogField fields[] = {
      {"reason", kStopReason},
      {"code", code.c_str()},
      {kOutcomeField, rl::OutcomeName(outcome)},
      {"consequence", pending ? kConsequencePending : kConsequenceConfirmed},
  };
  sink(rl::kLogLevelError, kRecordTag, kEventGameStop, nullptr, fields, sizeof fields / sizeof fields[0]);
}

// The APK's own runtime as the baseline release (StartContext::baseline): its version and sha256, read by the verifier from the folder
// the baseline is loaded from (Inputs::baseline_dir, else the shim's own folder). Not present when that file cannot be read or names no
// version: the engine then keeps its own answer for a root with no runtime.
engine::StartContext::BaselineRelease BaselineOf(const Inputs& in, const engine::ArtifactVerifier& verifier) {
  engine::StartContext::BaselineRelease out;
  std::string dir = in.baseline_dir;
  if (dir.empty()) {
    char buffer[kPathCapacity] = {};
    if (!rl::NativeLibraryDirOf(in.in_shim, buffer, sizeof buffer)) return out;
    dir = buffer;
  }
  const engine::ArtifactFacts facts = verifier.Measure(dir + kPathSeparator + names::kQuestRuntimeMember);
  if (!facts.present || facts.unreadable || facts.version.empty() || facts.sha256.empty()) return out;
  out.present = true;
  out.version = facts.version;
  out.sha256 = facts.sha256;
  return out;
}

Result StartChecked(const Inputs& in) {
  rl::HostInputs host = in.host;
  std::unique_ptr<engine::FsLock> lock;
  host.before_initialize = &ReleaseLock;
  host.before_initialize_arg = &lock;
  if (in.root.empty() || in.root.size() + 1 > kPathCapacity) {
    // No update root: nothing is read or written, and the start is the baseline alone.
    return Baseline(in, host, &lock);
  }
  std::snprintf(g_updateRoot, sizeof g_updateRoot, "%s", in.root.c_str());
  host.update_root = g_updateRoot;

  const std::unique_ptr<engine::UpdateFs> fs = engine::MakePosixUpdateFs(in.root);
  if (!fs) return Baseline(in, host, &lock);
  QuestVerifier verifier;
  QuestInUse in_use;
  QuestRollback rollback(verifier);
  QuestLoaderEdge loader;
  QuestLog log(in.host.log);
  QuestClock clock;
  engine::StartContext context{
      in.root,
      engine::StartEdges{*fs, verifier, in_use, rollback, loader, log, clock},
      engine::StartHooks{},
      engine::LockTiming{engine::kUpdateLockTimeoutMs, engine::kUpdateLockStepMs},
      &lock,
      std::string(names::kQuestRuntimeMember),
  };
  // The baseline release is read only when there is an update folder: a swap is the only thing that reads it.
  if (fs->IsDirectory(in.root + kPathSeparator + names::kUpdateDir)) context.baseline = BaselineOf(in, verifier);
  const engine::StartResult decided = engine::RunStart(context);
  // The swap capability is offered for every result but a stop (ADR 0024 section 6), kNoUpdateFolder included: a fresh install has no
  // update folder and needs the capability to stage its first release. An empty root never reaches here (Start returns the baseline).
  if (decided.action != engine::StartAction::kStop) host.capabilities = nevr_runtime_abi::kHostCapabilitySwap;
  if (decided.action != engine::StartAction::kLoad) {
    // No update folder, or a stop the engine recorded: the baseline runs, as the Quest never stops for lack of a
    // runtime.
    return Baseline(in, host, &lock);
  }

  const std::string path = in.root + kPathSeparator + names::kQuestRuntimeMember;
  bool pending = decided.pending;
  rl::Outcome outcome = rl::LoadUpdateFile(path.c_str(), host);
  if (outcome == rl::Outcome::kLoaded) return Updated(path);
  if (outcome == rl::Outcome::kHostContextRefused) {
    // The host did not hand the runtime what it needs (the shim handle, libr15's base or build ID): nothing ran, so nothing is judged.
    // The release is neither rolled back nor quarantined, and the baseline is not tried: it was given the same inputs.
    Result result;
    result.outcome = outcome;
    return result;
  }

  if (pending &&
      engine::ClassifyPendingFailure(ClassOf(outcome), kQuestCodeBeforeEntry) == engine::PendingFailure::kNeverRan) {
    // R1: no line of the pending runtime ran. The previous release is put back now, in this start, and loaded.
    const engine::StartResult restored = engine::RollBackNowIn(context, rl::OutcomeName(outcome));
    if (restored.action != engine::StartAction::kLoad) return Baseline(in, host, &lock);
    outcome = rl::LoadUpdateFile(path.c_str(), host);
    if (outcome == rl::Outcome::kLoaded) return Updated(path);
    pending = false;  // the release put back is not on trial: a failure of it is judged below, with nothing to mark
  }

  switch (engine::ClassifyPendingFailure(ClassOf(outcome), kQuestCodeBeforeEntry)) {
    case engine::PendingFailure::kMapped: {
      // R1: the runtime was mapped and its initialize failed, so it may have installed hooks. It is not run again in
      // this process: the failure is marked for the next start (pending), and the process ends with the stop code.
      if (pending) static_cast<void>(engine::MarkFailedIn(context, rl::OutcomeName(outcome)));
      Result result;
      result.outcome = outcome;
      result.stop = true;
      WriteStop(in.host.log, outcome, pending);
      return result;
    }
    case engine::PendingFailure::kNeverRan:
    case engine::PendingFailure::kNone:
      break;
  }
  return Baseline(in, host, &lock);
}

}  // namespace

Result Start(const Inputs& inputs) noexcept {
  try {
    return StartChecked(inputs);
  } catch (const std::exception&) {
    // Not even the start could be set up: the baseline loads, as it does without an update root.
    Result result;
    result.outcome = rl::StartBaselineBesideShim(inputs.in_shim, inputs.host);
    return result;
  }
}

}  // namespace nevr_quest::update_start
