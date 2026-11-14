// The self-update start's steps (docs/adr/0024-pc-self-update.md, sections 4 and 6), run on the Linux host against the POSIX
// edge. These are the step-injection cases of the Windows suite (test_host_update_swap F7, F20, F22) on the engine: a kill
// after every change to the disk of a swap and of a rollback, a journal that names outside the vocabulary, and a link in a
// folder the swap moves through. The Authenticode check, the in-use check, the rollback target and the load are fakes here:
// the PC edges are Win32, and the Quest ones are not written yet.
//
// The POSIX edge is nevr_update::MakePosixUpdateFs(root) (fs.h; fs_posix.cpp): one fs per start, rooted at the
// fixture folder, because every path must lie under the root it is made with.

#include "core/update/engine_start.h"
#include "core/update/fs.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace engine = nevr_update;
using json = nlohmann::json;

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr int kHexDigits = 16;
constexpr int kDigestRepeats = 4;
constexpr std::size_t kSha256HexLength = 64;
static_assert(kDigestRepeats * kHexDigits == static_cast<int>(kSha256HexLength), "the fake digest has a sha256's length");
constexpr std::uint32_t kTestLockTimeoutMs = 2000;
constexpr std::uint32_t kTestLockStepMs = 10;
// Every step a swap or a rollback makes in these fixtures is one of these: the kill is tried at each of them, and past the last
// one the start runs to its end.
constexpr int kKillStepsToTry = 40;
constexpr char kRuntime[] = "nevr-runtime.dll";
constexpr char kV1[] = "NEVR-BUILD 1.0.0\nrelease one\n";
constexpr char kV2[] = "NEVR-BUILD 2.0.0\nrelease two\n";
constexpr char kBuildMarker[] = "NEVR-BUILD ";

// The digest the fake verifier and the fixtures agree on: FNV-1a over the bytes, written as 64 lower-case hex digits.
std::string Digest(const std::string& bytes) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char c : bytes) {
    hash ^= c;
    hash *= kFnvPrime;
  }
  char hex[kHexDigits + 1] = {};
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
  std::string out;
  for (int i = 0; i < kDigestRepeats; ++i) out += hex;
  return out;
}

// The version a runtime fixture names in its NEVR-BUILD line, as the host reads it.
std::string VersionOf(const std::string& bytes) {
  const std::size_t at = bytes.find(kBuildMarker);
  if (at == std::string::npos) return {};
  const std::size_t from = at + sizeof(kBuildMarker) - 1;
  const std::size_t end = bytes.find_first_of(" \n", from);
  return bytes.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

class FakeVerifier final : public engine::ArtifactVerifier {
 public:
  engine::ArtifactCheck Verify(const std::string& path, const engine::ArtifactExpect& expect) const override {
    engine::ArtifactCheck check;
    if (!fs::exists(path)) {
      check.reason = "unreadable";
      return check;
    }
    const std::string bytes = Read(path);
    if (expect.check_digest && bytes.size() != expect.size) {
      check.reason = "size";
      return check;
    }
    if (expect.check_digest && Digest(bytes) != expect.sha256) {
      check.reason = "sha256";
      return check;
    }
    check.ok = true;
    return check;
  }

  engine::ArtifactFacts Measure(const std::string& path) const override {
    engine::ArtifactFacts facts;
    if (!fs::exists(path)) return facts;
    const std::string bytes = Read(path);
    facts.present = true;
    facts.sha256 = Digest(bytes);
    facts.version = VersionOf(bytes);
    return facts;
  }
};

class FakeInUse final : public engine::InUseCheck {
 public:
  engine::InUseAnswer Query(const std::string& runtime_path) const override {
    static_cast<void>(runtime_path);
    return {};  // nothing maps the runtime
  }
};

class FakeRollback final : public engine::RollbackTarget {
 public:
  explicit FakeRollback(const engine::ArtifactVerifier& verifier) : verifier_(verifier) {}

  engine::RollbackCheck Check(const std::string& runtime_path) const override {
    engine::RollbackCheck check;
    if (!fs::exists(runtime_path)) return check;  // kAbsent
    const engine::ArtifactCheck verdict = verifier_.Verify(runtime_path, engine::ArtifactExpect{});
    check.state = verdict.ok ? engine::RollbackState::kUsable : engine::RollbackState::kUnverified;
    check.reason = verdict.reason;
    return check;
  }

 private:
  const engine::ArtifactVerifier& verifier_;
};

class FakeLoader final : public engine::RuntimeLoaderEdge {
 public:
  bool CodeBeforeEntry() const noexcept override { return true; }
};

class FakeLog final : public engine::UpdateLog {
 public:
  void Emit(const json& record) override { records.push_back(record); }
  std::vector<json> records;
};

class FakeClock final : public engine::UpdateClock {
 public:
  std::uint64_t TickMs() const noexcept override { return 0; }
  std::string UtcIso() const override { return "2026-01-01T00:00:00Z"; }
};

// The POSIX edge with one change: its marker check (Exists) leaves an error of its own, as a platform's check may. Every other call
// clears that, so LastOsError answers the check's error right after a check, and the error of the last other call after that.
class MarkerCheckLeavesAnError final : public engine::UpdateFs {
 public:
  MarkerCheckLeavesAnError(std::unique_ptr<engine::UpdateFs> inner, int marker_error)
      : inner_(std::move(inner)), marker_error_(marker_error) {}

  char Separator() const noexcept override { return inner_->Separator(); }
  engine::ReadResult ReadSmallFile(const std::string& path, std::string* out) const noexcept override {
    Plain();
    return inner_->ReadSmallFile(path, out);
  }
  bool WriteByRename(const std::string& path, const std::string& text) const noexcept override {
    Plain();
    return inner_->WriteByRename(path, text);
  }
  bool RemoveFile(const std::string& path) const noexcept override {
    Plain();
    return inner_->RemoveFile(path);
  }
  void RemoveTree(const std::string& path) const noexcept override {
    Plain();
    inner_->RemoveTree(path);
  }
  bool IsLink(const std::string& path) const noexcept override {
    Plain();
    return inner_->IsLink(path);
  }
  bool Exists(const std::string& path) const noexcept override {
    const bool there = inner_->Exists(path);
    check_left_error_ = true;
    return there;
  }
  std::unique_ptr<engine::FsLock> TakeLock(const std::string& update_dir, std::uint32_t timeout_ms, std::uint32_t step_ms,
                                           int* os_error) const noexcept override {
    Plain();
    return inner_->TakeLock(update_dir, timeout_ms, step_ms, os_error);
  }
  bool Move(const std::string& from, const std::string& to, int* os_error) const noexcept override {
    Plain();
    return inner_->Move(from, to, os_error);
  }
  int LastOsError() const noexcept override { return check_left_error_ ? marker_error_ : inner_->LastOsError(); }
  bool MakeDir(const std::string& path) const noexcept override {
    Plain();
    return inner_->MakeDir(path);
  }
  bool RemoveEmptyDir(const std::string& path) const noexcept override {
    Plain();
    return inner_->RemoveEmptyDir(path);
  }
  bool IsDirectory(const std::string& path) const noexcept override {
    Plain();
    return inner_->IsDirectory(path);
  }
  bool ListFolder(const std::string& path, std::vector<engine::DirEntry>* out) const noexcept override {
    Plain();
    return inner_->ListFolder(path, out);
  }

 private:
  void Plain() const noexcept { check_left_error_ = false; }

  std::unique_ptr<engine::UpdateFs> inner_;
  int marker_error_;
  mutable bool check_left_error_ = false;
};

int g_kill_at = 0;  // the step the next start ends at (as a kill would); 0 for none
bool EndAt(int step) { return step == g_kill_at; }

// One start on `root` with the edges `fs` names, as the host makes it. `kill_at` ends it at that step of its disk changes.
engine::StartResult StartOn(const std::string& root, int kill_at, FakeLog* log, std::unique_ptr<engine::UpdateFs> edge) {
  g_kill_at = kill_at;
  FakeVerifier verifier;
  FakeInUse in_use;
  FakeRollback rollback(verifier);
  FakeLoader loader;
  FakeClock clock;
  std::unique_ptr<engine::FsLock> lock;
  engine::StartHooks hooks;
  hooks.step_ends = kill_at != 0 ? &EndAt : nullptr;
  const engine::StartContext context{root,
                                     engine::StartEdges{*edge, verifier, in_use, rollback, loader, *log, clock},
                                     hooks,
                                     engine::LockTiming{kTestLockTimeoutMs, kTestLockStepMs},
                                     &lock};
  const engine::StartResult result = engine::RunStart(context);
  g_kill_at = 0;
  return result;
}

// One start on `root`, on the POSIX edge. `kill_at` ends it at that step of its disk changes.
engine::StartResult Start(const std::string& root, int kill_at, FakeLog* log) {
  return StartOn(root, kill_at, log, engine::MakePosixUpdateFs(root));
}

// One start on `root`, on the POSIX edge whose marker check leaves `marker_error`.
engine::StartResult StartWithMarkerError(const std::string& root, FakeLog* log, int marker_error) {
  return StartOn(root, 0, log, std::make_unique<MarkerCheckLeavesAnError>(engine::MakePosixUpdateFs(root), marker_error));
}

bool HasRecord(const FakeLog& log, const std::string& event, const std::string& outcome, const std::string& reason) {
  for (const json& record : log.records) {
    if (record.value("event", std::string()) == event && record.value("outcome", std::string()) == outcome &&
        record.value("reason", std::string()) == reason) {
      return true;
    }
  }
  return false;
}

// The first record with this reason, or null.
const json* FindRecord(const FakeLog& log, const std::string& reason) {
  for (const json& record : log.records) {
    if (record.value("reason", std::string()) == reason) return &record;
  }
  return nullptr;
}

// The fixtures live under the system temporary directory (TMPDIR), not the checkout: ctest runs this test in the repository root.
fs::path FixtureRoot(const std::string& name) {
  const fs::path root = fs::temp_directory_path() / "nevr-engine-start-steps" / name;
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

// A swap ready to run: v1 in the root, v2 staged and listed in staged.json.
fs::path SwapFixture(const std::string& name) {
  const fs::path root = FixtureRoot(name);
  Write(root / kRuntime, kV1);
  Write(root / "update" / "staging" / "2.0.0" / kRuntime, kV2);
  const json file = {{"path", kRuntime}, {"size", std::string(kV2).size()}, {"sha256", Digest(kV2)}};
  const json staged = {{"version", "2.0.0"}, {"files", json::array({file})}};
  Write(root / "update" / "staged.json", staged.dump());
  return root;
}

// A rollback ready to run: v2 pending in the root with failed_reason set, v1 in previous/1.0.0.
fs::path RollbackFixture(const std::string& name) {
  const fs::path root = FixtureRoot(name);
  Write(root / kRuntime, kV2);
  Write(root / "update" / "previous" / "1.0.0" / kRuntime, kV1);
  const json pending = {{"to", {{"version", "2.0.0"}, {"sha256", Digest(kV2)}}},
                        {"from", {{"version", "1.0.0"}, {"sha256", Digest(kV1)}}},
                        {"boot_attempts", 1},
                        {"failed_reason", "init_failed"}};
  Write(root / "update" / "pending.json", pending.dump());
  return root;
}

// F7: the swap is interrupted at any step; the root never holds a mix, and the next start finishes the release.
TEST(EngineStartSteps, AKillAtAnyStepOfASwapLeavesAWholeReleaseAndTheNextStartFinishesIt) {
  for (int step = 1; step <= kKillStepsToTry; ++step) {
    const fs::path root = SwapFixture("f7-swap-" + std::to_string(step));
    FakeLog killed;
    Start(root.string(), step, &killed);
    const fs::path runtime = root / kRuntime;
    if (fs::exists(runtime)) {
      const std::string bytes = Read(runtime);
      EXPECT_TRUE(bytes == kV1 || bytes == kV2) << "a mix in the root after a kill at step " << step;
    }
    FakeLog log;
    Start(root.string(), 0, &log);
    EXPECT_EQ(Read(runtime), kV2) << "the release is not whole after a kill at step " << step;
    EXPECT_TRUE(fs::exists(root / "update" / "pending.json")) << "step " << step;
    EXPECT_FALSE(fs::exists(root / "update" / "staged.json")) << "step " << step;
    EXPECT_FALSE(fs::exists(root / "update" / "staging" / "2.0.0")) << "the staging folder is left after a completed swap, step " << step;
    EXPECT_FALSE(fs::exists(root / "update" / "swap.json")) << "step " << step;
  }
}

// F20: a rollback is interrupted; the next start completes it and the failed release does not come back.
TEST(EngineStartSteps, AKillAtAnyStepOfARollbackIsCompletedAndTheFailedReleaseDoesNotReturn) {
  for (int step = 1; step <= kKillStepsToTry; ++step) {
    const fs::path root = RollbackFixture("f20-rollback-" + std::to_string(step));
    FakeLog first;
    Start(root.string(), step, &first);
    FakeLog second;
    const engine::StartResult result = Start(root.string(), 0, &second);
    EXPECT_EQ(result.action, engine::StartAction::kLoad) << "step " << step;
    EXPECT_EQ(Read(root / kRuntime), kV1) << "the previous release is not back after a kill at step " << step;
    EXPECT_FALSE(fs::exists(root / "update" / "pending.json")) << "step " << step;
    EXPECT_FALSE(fs::exists(root / "update" / "swap.json")) << "step " << step;
    EXPECT_TRUE(fs::exists(root / "update" / "failed" / "2.0.0" / kRuntime)) << "step " << step;
    const json state = json::parse(Read(root / "update" / "state.json"));
    EXPECT_NE(state.dump().find("\"2.0.0\""), std::string::npos) << "the failed release is not quarantined at step " << step;
  }
}

// F22: a journal that names a version outside the vocabulary is not executed: the start stops and the root is untouched.
TEST(EngineStartSteps, AJournalNamingOutsideTheVocabularyStopsTheStartWithoutTouchingTheRoot) {
  const fs::path root = SwapFixture("f22-bad-journal");
  Write(root / "update" / "swap.json",
        json({{"kind", "swap"}, {"from", "../x"}, {"to", "2.0.0"}, {"phase", "out"}, {"out", json::array()}, {"in", json::array()}})
            .dump());
  FakeLog log;
  const engine::StartResult result = Start(root.string(), 0, &log);
  EXPECT_EQ(result.action, engine::StartAction::kStop);
  ASSERT_NE(result.reason, nullptr);
  EXPECT_STREQ(result.reason, "bad_journal");
  EXPECT_EQ(Read(root / kRuntime), kV1);
  EXPECT_TRUE(HasRecord(log, "update_swap", "blocked", "bad_journal"));
}

// F22: a link where the swap would move through a folder is refused, and the record names the folder.
TEST(EngineStartSteps, ALinkInTheUpdateFoldersIsRefusedAndNamedInTheRecord) {
  const fs::path root = SwapFixture("f22-link");
  const fs::path elsewhere = FixtureRoot("f22-link-elsewhere");
  fs::create_directories(elsewhere);
  fs::create_directory_symlink(elsewhere, root / "update" / "previous");
  FakeLog log;
  const engine::StartResult result = Start(root.string(), 0, &log);
  EXPECT_EQ(result.action, engine::StartAction::kLoad);
  EXPECT_EQ(Read(root / kRuntime), kV1);
  EXPECT_TRUE(HasRecord(log, "update_swap", "blocked", "reparse_point"));
}

// A pending record that cannot be read is left alone, and the record of that read names the read's error. The marker check after the
// read leaves another error on this edge, so a record that took its error after the check would name ENOENT instead of ELOOP.
TEST(EngineStartSteps, APendingRecordThatCannotBeReadKeepsTheErrorOfItsRead) {
  const fs::path root = FixtureRoot("pending-unreadable");
  Write(root / kRuntime, kV1);
  Write(root / "update" / "elsewhere.json", "{}");
  fs::create_symlink(root / "update" / "elsewhere.json", root / "update" / "pending.json");
  FakeLog log;
  const engine::StartResult result = StartWithMarkerError(root.string(), &log, ENOENT);
  EXPECT_EQ(result.action, engine::StartAction::kLoad);
  EXPECT_EQ(Read(root / kRuntime), kV1);
  const json* record = FindRecord(log, "pending_unreadable");
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->value("win32_error", 0), ELOOP);
}

// A swap removes the older generation before it moves the old release in: update\previous\0.9.0 (holding a runtime) is gone
// afterwards, and previous\1.0.0 holds the release the swap moved out. The clearing reads the generation folders through
// ListFolder's is_dir (engine_start.cpp, ClearGenerations), so this case fails when is_dir is wrong.
TEST(EngineStartSteps, ASwapRemovesTheOlderPreviousGenerationAndKeepsTheOneItMoves) {
  const fs::path root = SwapFixture("f3-previous-generation");
  Write(root / "update" / "previous" / "0.9.0" / kRuntime, "NEVR-BUILD 0.9.0\nrelease zero\n");
  FakeLog log;
  const engine::StartResult result = Start(root.string(), 0, &log);
  EXPECT_EQ(result.action, engine::StartAction::kLoad);
  EXPECT_TRUE(result.pending) << "a release that was just swapped in is pending";
  EXPECT_EQ(Read(root / kRuntime), kV2);
  EXPECT_FALSE(fs::exists(root / "update" / "previous" / "0.9.0")) << "the older generation is left after a swap";
  EXPECT_EQ(Read(root / "update" / "previous" / "1.0.0" / kRuntime), kV1);
}

}  // namespace
