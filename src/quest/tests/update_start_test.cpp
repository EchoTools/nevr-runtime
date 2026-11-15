// Host test for the Quest's update start (src/quest/shim/update_start.h) on real files under the Linux host: the
// engine's decision over a POSIX update root, the swap of a staged fixture runtime, the baseline when there is no
// update folder, a corrupt staging, a runtime whose entry is refused (put back in the same start, or the baseline), and
// a runtime whose initialize fails (the stop code, and the failure marked for the next start). The fixtures are the
// runtime_load fixtures (runtime_load_fixture.cpp), copied in as libnevr-runtime.so, so each start maps and initializes
// a real shared object.
//
// Run: update_start_test <fixture-dir>   (the integration group's build directory: libnevr_fixture_<mode>.so)
#include "quest/shim/update_start.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "core/runtime_abi_common.h"
#include "core/update/edges.h"
#include "core/update/engine_start.h"
#include "core/update/fs.h"
#include "core/update/names.h"
#include "quest/data/sha256.h"
#include "quest/shim/runtime_load.h"
#include "quest/tests/test_check.h"

namespace {

namespace fs = std::filesystem;
namespace us = nevr_quest::update_start;
namespace rl = nevr_quest::runtime_load;
using json = nlohmann::json;

constexpr int kArgCount = 2;
constexpr int kUsageExit = 2;
constexpr std::uint64_t kSwapCapability = nevr_runtime_abi::kHostCapabilitySwap;
constexpr std::uint64_t kNoCapability = 0;
constexpr int kUpdateFailedExitCodeExpected = 78;  // the stop code of a failed update start (ADR 0024)
constexpr int kFailedExit = 1;
constexpr char kBuildId[] = "0123456789abcdef0123456789abcdef01234567";
constexpr char kShimVersion[] = "6.18.0-test";
constexpr std::uint32_t kVariant = 3;
constexpr char kRuntimeFile[] = "libnevr-runtime.so";
constexpr char kStagedJson[] = "staged.json";
constexpr char kPendingJson[] = "pending.json";
constexpr char kUpdateDir[] = "update";
constexpr char kStagingDir[] = "staging";
constexpr char kPreviousDir[] = "previous";
constexpr char kFailedDir[] = "failed";
constexpr char kStateJson[] = "state.json";
constexpr char kSeedVersion[] =
    "6.18.0";  // the version the fixtures' NEVR-BUILD literal names (runtime_load_fixture.cpp)
constexpr char kOutcomeNullDescriptor[] = "null_descriptor";
constexpr char kPcRuntimeFile[] = "nevr-runtime.dll";
constexpr char kPluginFile[] = "plugins/extra.dll";
constexpr char kReasonBadStaged[] = "bad_staged";
// The fixture's NEVR_FixtureLastContext, and the value CapabilitiesSeenBy returns when it cannot read one.
using NevrQuestLastContextFn = const NevrQuestHostContext* (*)();
constexpr std::uint64_t kNoRecord = ~std::uint64_t{0};
constexpr char kOutcomeAbsent[] = "absent";
constexpr char kVersionOne[] = "2.0.0";
constexpr char kVersionTwo[] = "3.0.0";
constexpr char kFieldAbsent[] = "<absent>";
constexpr char kEventSwap[] = "update_swap";
constexpr char kEventRollback[] = "update_rollback";
constexpr char kEventStop[] = "game_stop";
constexpr char kEventRuntimeLoad[] = "runtime_load";
constexpr char kOutcomeSwapped[] = "swapped";
constexpr char kOutcomeDiscarded[] = "discarded";
constexpr char kOutcomeRolledBack[] = "rolled_back";
constexpr char kSourceUpdate[] = "update";
constexpr char kSourceBaseline[] = "baseline";
constexpr char kFailedReasonKey[] = "failed_reason";
constexpr char kOutcomeInitFailed[] = "init_failed";
constexpr char kReasonUpdateFailed[] = "update_failed";
constexpr char kBaseDirName[] = "base";
constexpr char kWrongDigest[] = "0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::uint32_t kFieldCount = 16;  // the most fields a record of the start carries
constexpr char kShimDirName[] = "shim";
constexpr char kShimFile[] = "libovrplatformloader.so";  // the shim's file name in the APK
constexpr char kOutcomeLoaded[] = "loaded";
constexpr char kOutcomeHostRefused[] = "host_context_refused";
constexpr char kBootAttemptsKey[] = "boot_attempts";
constexpr std::uint64_t kNoTryCounted = 0;
constexpr std::uint64_t kFirstTry = 1;
constexpr std::uint64_t kNoBootAttempts = ~std::uint64_t{0};
constexpr int kRefusedStarts = 2;
constexpr std::uint64_t kAttemptsOnRecord = 2;  // a pending record on its second try
constexpr std::uint64_t kAnotherCount = 3;      // a count this start did not write
constexpr std::uint32_t kShortLockWaitMs = 50;  // a lock wait that ends before the holder lets go
constexpr std::uint32_t kLockStepMs = 10;
constexpr char kGiveBackReason[] = "host_context_refused";
constexpr fs::perms kReadOnlyDir = fs::perms::owner_read | fs::perms::owner_exec;  // the swap's start, then one start on trial

struct Captured {
  std::uint32_t level = 0;
  std::string event;
  std::vector<std::pair<std::string, std::string>> fields;

  std::string Field(const char* key) const {
    for (const auto& field : fields) {
      if (field.first == key) return field.second;
    }
    return kFieldAbsent;
  }
};

std::vector<Captured> g_records;
std::string g_dir;
char g_anchor = 0;  // stands for the shim's handle and the libr15 base: any non-null address the test owns

void CaptureSink(std::uint32_t level, const char* tag, const char* event, const char* message,
                 const NevrQuestLogField* fields, std::uint32_t count) {
  static_cast<void>(tag);
  static_cast<void>(message);
  Captured record;
  record.level = level;
  record.event = event;
  for (std::uint32_t i = 0; i < count && i < kFieldCount; ++i)
    record.fields.emplace_back(fields[i].key, fields[i].value);
  g_records.push_back(record);
}

// The last record with `event` and, when `outcome` is not empty, that outcome. Null when there is none.
const Captured* Find(const char* event, const char* outcome = "") {
  for (auto it = g_records.rbegin(); it != g_records.rend(); ++it) {
    if (it->event != event) continue;
    if (outcome[0] != '\0' && it->Field("outcome") != outcome) continue;
    return &*it;
  }
  return nullptr;
}

rl::HostInputs HostInputsForTest() {
  rl::HostInputs host;
  host.shim_handle = &g_anchor;
  host.libr15_base = &g_anchor;
  host.libr15_build_id = kBuildId;
  host.shim_version = kShimVersion;
  host.variant = kVariant;
  host.log = &CaptureSink;
  return host;
}

std::string FixturePath(const char* mode) { return g_dir + "/libnevr_fixture_" + mode + ".so"; }

std::string ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << bytes;
}

std::string Digest(const std::string& bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

// A fresh root for one case: the case's folder is removed first, so no earlier case's update folder is seen.
fs::path CaseRoot(const char* name) {
  const fs::path base = fs::path(g_dir) / name;
  std::error_code ignored;
  fs::remove_all(base, ignored);
  fs::create_directories(base / "nevr");
  return base;
}

// The baseline folder of a case: it holds the fixture the baseline start loads.
std::string BaselineDir(const fs::path& base, const char* mode) {
  const fs::path dir = base / kBaseDirName;
  WriteAll(dir / kRuntimeFile, ReadAll(FixturePath(mode)));
  return dir.string();
}

// Puts a runtime the root already owns there: the file, and update\state.json naming it as owned (a swap moves only
// what it owns).
void SeedOwnedRuntime(const fs::path& root, const char* mode) {
  const std::string bytes = ReadAll(FixturePath(mode));
  WriteAll(root / kRuntimeFile, bytes);
  const json state = {{"owned", json::array({json{{"path", kRuntimeFile}, {"sha256", Digest(bytes)}}})}};
  WriteAll(root / kUpdateDir / kStateJson, state.dump());
}

// Stages a release's runtime (a copy of the fixture) with its staged.json entry, as the runtime's check would publish
// it. A corrupt staging names a digest the bytes do not have.
void StageRelease(const fs::path& root, const char* version, const char* mode, bool corrupt) {
  const std::string bytes = ReadAll(FixturePath(mode));
  WriteAll(root / kUpdateDir / kStagingDir / version / kRuntimeFile, bytes);
  const json staged = {
      {"version", version},
      {"files",
       json::array(
           {json{{"path", kRuntimeFile}, {"size", bytes.size()}, {"sha256", corrupt ? kWrongDigest : Digest(bytes)}}})},
  };
  WriteAll(root / kUpdateDir / kStagedJson, staged.dump());
}

// glibc returns an object already loaded under the same name, and a load by descriptor names the object
// /proc/self/fd/<n>. Every start opens the runtime at a descriptor number no earlier start used (the runtime_load_test
// technique): the numbers below the high-water mark stay open for the rest of the run.
int g_highWater = 0;
void UseAFreshDescriptorNumber() {
  for (;;) {
    const int probe = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    QCHECK(probe >= 0);
    if (probe < 0) return;
    if (probe > g_highWater) {
      g_highWater = probe;
      ::close(probe);
      return;
    }
  }
}

us::Inputs InputsFor(const fs::path& root, const std::string& baseline_dir) {
  us::Inputs in;
  in.root = root.string();
  in.baseline_dir = baseline_dir;
  in.host = HostInputsForTest();
  return in;
}

us::Result StartCase(const us::Inputs& in) {
  UseAFreshDescriptorNumber();
  return us::Start(in);
}

std::uint64_t CapabilitiesSeenBy(const std::string& path);
const NevrQuestHostContext* ContextSeenBy(const std::string& path);

// A stand-in for the shim, as production has it: a shared object the process has dlopen'd from its own folder, which
// holds the APK's baseline too. The object is a copy of a fixture (any shared object serves), so the address returned
// lies inside it and dladdr names its file. Null when it cannot be made.
const void* AddressInAShim(const fs::path& base) {
  const fs::path dir = base / kShimDirName;
  WriteAll(dir / kShimFile, ReadAll(FixturePath("ok")));
  WriteAll(dir / kRuntimeFile, ReadAll(FixturePath("ok")));
  void* handle = ::dlopen((dir / kShimFile).c_str(), RTLD_NOW | RTLD_LOCAL);
  QCHECK(handle != nullptr);
  if (handle == nullptr) return nullptr;
  return ::dlsym(handle, nevr_runtime_abi::kGetRuntimeApiExport);
}

// The boot_attempts of update\pending.json, or kNoBootAttempts when there is no such record.
std::uint64_t BootAttempts(const fs::path& root) {
  const json pending = json::parse(ReadAll(root / kUpdateDir / kPendingJson), nullptr, false);
  if (!pending.is_object() || !pending.contains(kBootAttemptsKey) || !pending[kBootAttemptsKey].is_number_unsigned())
    return kNoBootAttempts;
  return pending[kBootAttemptsKey].get<std::uint64_t>();
}

// A case with no update folder: the start is the baseline, and nothing is written under the root.
bool BaselineWithoutUpdateFolder() {
  const fs::path base = CaseRoot("baseline-without-update");
  g_records.clear();
  const us::Result result = StartCase(InputsFor(base / "nevr", BaselineDir(base, "ok")));
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kBaseline);
  QCHECK(!result.stop);
  QCHECK(!fs::exists(base / "nevr" / kUpdateDir));
  const Captured* load = Find(kEventRuntimeLoad);
  QCHECK(load != nullptr && load->Field("source") == kSourceBaseline);
  // kNoUpdateFolder is a decision: a fresh install needs the capability to stage its first release.
  QCHECK(CapabilitiesSeenBy((base / kBaseDirName / kRuntimeFile).string()) == kSwapCapability);
  return true;
}

// A root string that names a directory which does not exist is still a decision (kNoUpdateFolder): the baseline runs
// and the runtime is offered the swap capability, as the capability only needs a non-empty root.
bool MissingRootDirStillOffersSwapCapability() {
  const fs::path base = CaseRoot("missing-root-dir");
  g_records.clear();
  const us::Result result = StartCase(InputsFor(base / "absent" / "nevr", BaselineDir(base, "ok")));
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kBaseline);
  QCHECK(!fs::exists(base / "absent"));
  QCHECK(CapabilitiesSeenBy((base / kBaseDirName / kRuntimeFile).string()) == kSwapCapability);
  return true;
}

// An engine stop (a journal left in update\ and a linked staging folder: the root may be half moved) loads the
// baseline with no swap capability: nothing was decided.
bool EngineStopOffersNoSwapCapability() {
  const fs::path base = CaseRoot("engine-stop");
  g_records.clear();
  const fs::path root = base / "nevr";
  SeedOwnedRuntime(root, "null_descriptor");
  WriteAll(root / kUpdateDir / "swap.json", "{}");
  fs::create_directory_symlink(base, root / kUpdateDir / kStagingDir);
  const std::string baseline = BaselineDir(base, "ok");
  const us::Result result = StartCase(InputsFor(root, baseline));
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kBaseline);
  const Captured* blocked = Find("update_swap", "blocked");
  QCHECK(blocked != nullptr && blocked->Field("reason") == "reparse_point");
  QCHECK(CapabilitiesSeenBy((fs::path(baseline) / kRuntimeFile).string()) == kNoCapability);
  return true;
}

// A staged runtime is swapped over the runtime the root already owns, and the new one loads from the root. The old one
// moves to previous\<from>\, named by its version (the seeded fixture's literal).
bool StagedRuntimeIsSwappedInAndLoaded() {
  const fs::path base = CaseRoot("swap");
  g_records.clear();
  const fs::path root = base / "nevr";
  SeedOwnedRuntime(root, "null_descriptor");
  StageRelease(root, kVersionOne, "ok", false);
  const us::Result result = StartCase(InputsFor(root, BaselineDir(base, "ok")));
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kUpdate);
  QCHECK(result.path == (root / kRuntimeFile).string());
  QCHECK(Digest(ReadAll(root / kRuntimeFile)) == Digest(ReadAll(FixturePath("ok"))));
  QCHECK(Digest(ReadAll(root / kUpdateDir / kPreviousDir / kSeedVersion / kRuntimeFile)) ==
         Digest(ReadAll(FixturePath("null_descriptor"))));
  QCHECK(fs::exists(root / kUpdateDir / kPendingJson));
  QCHECK(Find(kEventSwap, kOutcomeSwapped) != nullptr);
  const Captured* load = Find(kEventRuntimeLoad);
  QCHECK(load != nullptr && load->Field("source") == kSourceUpdate);
  QCHECK(load != nullptr && load->Field("outcome") == "loaded");
  QCHECK(CapabilitiesSeenBy(result.path) == kSwapCapability);
  return true;
}

// A staging whose bytes do not match its entry is discarded by the engine. The runtime the root already has is then the
// one that loads, and it is refused at its entry with nothing pending: the start loads the baseline.
bool CorruptStagingIsDiscarded() {
  const fs::path base = CaseRoot("corrupt-staging");
  g_records.clear();
  const fs::path root = base / "nevr";
  SeedOwnedRuntime(root, "null_descriptor");
  StageRelease(root, kVersionOne, "ok", true);
  const us::Result result = StartCase(InputsFor(root, BaselineDir(base, "ok")));
  QCHECK(!result.stop);
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kBaseline);
  QCHECK(Digest(ReadAll(root / kRuntimeFile)) == Digest(ReadAll(FixturePath("null_descriptor"))));
  QCHECK(Find(kEventSwap, kOutcomeDiscarded) != nullptr);
  const Captured* load = Find(kEventRuntimeLoad, kOutcomeNullDescriptor);
  QCHECK(load != nullptr && load->Field("source") == kSourceUpdate);
  return true;
}

// A fresh root (no runtime yet: the APK carries the baseline): the first staged release is swapped in, and the pending record names the
// baseline as the release it replaces.
bool FreshRootSwapsTheFirstRelease() {
  const fs::path base = CaseRoot("fresh-swap");
  g_records.clear();
  const fs::path root = base / "nevr";
  StageRelease(root, kVersionOne, "ok", false);
  const us::Result result = StartCase(InputsFor(root, BaselineDir(base, "ok")));
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kUpdate);
  QCHECK(Digest(ReadAll(root / kRuntimeFile)) == Digest(ReadAll(FixturePath("ok"))));
  const json pending = json::parse(ReadAll(root / kUpdateDir / kPendingJson), nullptr, false);
  QCHECK(pending.is_object() && pending.contains("from") && pending["from"].is_object());
  QCHECK(pending.is_object() && pending.contains("from") && pending["from"].value("version", std::string()) == kSeedVersion);
  return true;
}

// A fresh root whose first release never runs: the rollback quarantines the failed runtime (out of the root, into failed\) and the
// start loads the baseline. The start does not stop.
bool FreshRootFailedReleaseLoadsTheBaseline() {
  const fs::path base = CaseRoot("fresh-failed");
  g_records.clear();
  const fs::path root = base / "nevr";
  StageRelease(root, kVersionOne, "no_entry", false);
  const us::Result result = StartCase(InputsFor(root, BaselineDir(base, "ok")));
  QCHECK(!result.stop);
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kBaseline);
  QCHECK(!fs::exists(root / kRuntimeFile));
  QCHECK(fs::exists(root / kUpdateDir / kFailedDir / kVersionOne / kRuntimeFile));
  QCHECK(Find(kEventRollback, kOutcomeRolledBack) != nullptr);
  return true;
}

// The capabilities word the fixture at `path` was handed by its last initialize, read through its record (runtime_load_fixture.cpp).
// The object is already loaded under that file, and RTLD_NOLOAD finds it by its file identity. kNoRecord when there is none.
std::uint64_t CapabilitiesSeenBy(const std::string& path) {
  const NevrQuestHostContext* context = ContextSeenBy(path);
  return context == nullptr ? kNoRecord : context->capabilities;
}

// The context the fixture at `path` was handed by its last initialize; null when there is none.
const NevrQuestHostContext* ContextSeenBy(const std::string& path) {
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (handle == nullptr) return nullptr;
  void* symbol = ::dlsym(handle, "NEVR_FixtureLastContext");
  if (symbol == nullptr) return nullptr;
  return reinterpret_cast<NevrQuestLastContextFn>(symbol)();
}

// The host did not hand the runtime its libr15 base: the pending release is judged by nothing. It is not rolled back and not
// quarantined, and the start does not stop. The release that was swapped in stays in the root, pending.
bool HostRefusalDoesNotJudgeTheRelease() {
  const fs::path base = CaseRoot("host-refused");
  g_records.clear();
  const fs::path root = base / "nevr";
  SeedOwnedRuntime(root, "null_descriptor");
  StageRelease(root, kVersionTwo, "ok", false);
  us::Inputs in = InputsFor(root, BaselineDir(base, "ok"));
  in.host.libr15_base = nullptr;
  const us::Result result = StartCase(in);
  QCHECK(result.outcome == rl::Outcome::kHostContextRefused);
  QCHECK(!result.stop);
  QCHECK(Find(kEventRollback) == nullptr);
  QCHECK(!fs::exists(root / kUpdateDir / kFailedDir / kVersionTwo));
  QCHECK(Digest(ReadAll(root / kRuntimeFile)) == Digest(ReadAll(FixturePath("ok"))));
  const json pending = json::parse(ReadAll(root / kUpdateDir / kPendingJson), nullptr, false);
  QCHECK(pending.is_object() && !(pending.contains(kFailedReasonKey) && pending[kFailedReasonKey].is_string()));
  return true;
}

// The shim's own start: no baseline folder is given and no shim handle is set (entry.cpp sets neither); the start
// takes both from the address inside the shim. A staged release is swapped in and loads from the update root, and the
// runtime is handed the shim's handle.
bool ProductionShapeLoadsTheUpdate() {
  const fs::path base = CaseRoot("production-shape");
  g_records.clear();
  const fs::path root = base / "nevr";
  SeedOwnedRuntime(root, "null_descriptor");
  StageRelease(root, kVersionTwo, "ok", false);
  us::Inputs in = InputsFor(root, std::string());
  in.host.shim_handle = nullptr;
  in.in_shim = AddressInAShim(base);
  const us::Result result = StartCase(in);
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kUpdate);
  QCHECK(result.path == (root / kRuntimeFile).string());
  const NevrQuestHostContext* context = ContextSeenBy(result.path);
  QCHECK(context != nullptr && context->shim_handle != nullptr);
  const Captured* load = Find(kEventRuntimeLoad, kOutcomeLoaded);
  QCHECK(load != nullptr && load->Field("source") == kSourceUpdate);
  QCHECK(Find(kEventRuntimeLoad, kOutcomeHostRefused) == nullptr);
  return true;
}

// A host input the update load is refused for judges the host, not the release: the start goes on to the baseline (the
// game does not run without a runtime), and the try this start counted is given back, so a release the host never
// started is not rolled back for want of starts. The swap's start and a later start on trial both give theirs back; a
// start with the inputs whole then takes its first try.
bool HostRefusalFallsBackToTheBaselineAndCountsNoTry() {
  const fs::path base = CaseRoot("host-refused-fallback");
  const fs::path root = base / "nevr";
  SeedOwnedRuntime(root, "null_descriptor");
  StageRelease(root, kVersionTwo, "ok", false);
  const std::string baseline = BaselineDir(base, "ok");
  for (int start = 0; start < kRefusedStarts; ++start) {
    g_records.clear();
    us::Inputs in = InputsFor(root, baseline);
    in.host.libr15_base = nullptr;
    const us::Result result = StartCase(in);
    QCHECK(!result.stop);
    QCHECK(result.origin == us::Origin::kBaseline);
    // The update load is refused first, and the baseline's load follows it.
    bool update_refused = false;
    bool baseline_tried = false;
    for (const Captured& record : g_records) {
      if (record.event != kEventRuntimeLoad) continue;
      if (record.Field("source") == kSourceUpdate && record.Field("outcome") == kOutcomeHostRefused) update_refused = true;
      if (record.Field("source") == kSourceBaseline && update_refused) baseline_tried = true;
    }
    QCHECK(update_refused);
    QCHECK(baseline_tried);
    const Captured* given_back = Find("update_trial", "not_counted");
    QCHECK(given_back != nullptr && given_back->Field("reason") == kOutcomeHostRefused);
    QCHECK(Find(kEventRollback) == nullptr);
    QCHECK(BootAttempts(root) == kNoTryCounted);
  }
  g_records.clear();
  const us::Result result = StartCase(InputsFor(root, baseline));
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kUpdate);
  QCHECK(BootAttempts(root) == kFirstTry);
  return true;
}

// The engine edges a give-back uses: only the file system, the log and the clock are asked; the rest answer nothing.
class NoVerifier final : public nevr_update::ArtifactVerifier {
 public:
  nevr_update::ArtifactCheck Verify(const std::string&, const nevr_update::ArtifactExpect&) const override { return {}; }
  nevr_update::ArtifactFacts Measure(const std::string&) const override { return {}; }
};
class NoInUse final : public nevr_update::InUseCheck {
 public:
  nevr_update::InUseAnswer Query(const std::string&) const override { return {}; }
};
class NoRollback final : public nevr_update::RollbackTarget {
 public:
  nevr_update::RollbackCheck Check(const std::string&) const override { return {}; }
};
class NoLoader final : public nevr_update::RuntimeLoaderEdge {
 public:
  bool CodeBeforeEntry() const noexcept override { return false; }
};
class JsonCapture final : public nevr_update::UpdateLog {
 public:
  void Emit(const json& record) override { records.push_back(record); }
  std::vector<json> records;
};
class ZeroClock final : public nevr_update::UpdateClock {
 public:
  std::uint64_t TickMs() const noexcept override { return 0; }
  std::string UtcIso() const override { return std::string(); }
};

// Writes a pending record for `attempts` tries.
void WritePending(const fs::path& root, std::uint64_t attempts) {
  const json pending = {{"to", {{"version", kVersionTwo}, {"sha256", kWrongDigest}}},
                        {"from", nullptr},
                        {kBootAttemptsKey, attempts},
                        {kFailedReasonKey, nullptr}};
  WriteAll(root / kUpdateDir / kPendingJson, pending.dump());
}

// UncountTryIn gives back only the try this start wrote: a count of 0 and a record that holds another count are left as
// they are, a lock it cannot take and a record it cannot write are said in an update_trial record that keeps the count,
// and the start's own count goes down by one with one not_counted record.
bool UncountTryGivesBackOnlyThisStartsTry() {
  const fs::path base = CaseRoot("uncount");
  const fs::path root = base / "nevr";
  const std::unique_ptr<nevr_update::UpdateFs> ufs = nevr_update::MakePosixUpdateFs(root.string());
  QCHECK(ufs != nullptr);
  if (!ufs) return false;
  NoVerifier verifier;
  NoInUse in_use;
  NoRollback rollback;
  NoLoader loader;
  JsonCapture log;
  ZeroClock clock;
  std::unique_ptr<nevr_update::FsLock> lock;
  const nevr_update::StartContext context{
      root.string(),
      nevr_update::StartEdges{*ufs, verifier, in_use, rollback, loader, log, clock},
      nevr_update::StartHooks{},
      nevr_update::LockTiming{kShortLockWaitMs, kLockStepMs},
      &lock,
      std::string(kRuntimeFile),
  };
  const auto trial = [&log](const char* outcome) {
    for (const json& record : log.records)
      if (record.value("event", "") == "update_trial" && record.value("outcome", "") == outcome) return &record;
    return static_cast<const json*>(nullptr);
  };

  // A start that counted nothing gives nothing back, even from a record of 0 tries.
  WritePending(root, kNoTryCounted);
  QCHECK(!nevr_update::UncountTryIn(context, kNoTryCounted, kGiveBackReason));
  QCHECK(BootAttempts(root) == kNoTryCounted);
  // A record that holds another count is not this start's.
  WritePending(root, kAttemptsOnRecord);
  QCHECK(!nevr_update::UncountTryIn(context, kAnotherCount, kGiveBackReason));
  QCHECK(BootAttempts(root) == kAttemptsOnRecord);
  QCHECK(log.records.empty());
  // update\lock held by another taker past the wait: nothing is written, and the record says the try stays counted. The
  // slot lets go of the lock the call above took first.
  lock.reset();
  {
    int os = 0;
    std::unique_ptr<nevr_update::FsLock> other =
        ufs->TakeLock((root / kUpdateDir).string(), kShortLockWaitMs, kLockStepMs, &os);
    QCHECK(other != nullptr);
    QCHECK(!nevr_update::UncountTryIn(context, kAttemptsOnRecord, kGiveBackReason));
  }
  QCHECK(BootAttempts(root) == kAttemptsOnRecord);
  const json* timed_out = trial("counted");
  QCHECK(timed_out != nullptr && timed_out->value("reason", "") == "lock_timeout");
  QCHECK(timed_out != nullptr && !timed_out->contains("release"));
  log.records.clear();
  // A record that cannot be written keeps its count, and says so.
  fs::permissions(root / kUpdateDir, kReadOnlyDir);
  QCHECK(!nevr_update::UncountTryIn(context, kAttemptsOnRecord, kGiveBackReason));
  fs::permissions(root / kUpdateDir, fs::perms::owner_all);
  QCHECK(BootAttempts(root) == kAttemptsOnRecord);
  const json* unwritten = trial("counted");
  QCHECK(unwritten != nullptr && unwritten->value("reason", "") == "pending_write_failed");
  log.records.clear();
  // No reason: nothing.
  QCHECK(!nevr_update::UncountTryIn(context, kAttemptsOnRecord, nullptr));
  QCHECK(BootAttempts(root) == kAttemptsOnRecord);
  // This start's own count: one less, and one not_counted record that says why.
  QCHECK(nevr_update::UncountTryIn(context, kAttemptsOnRecord, kGiveBackReason));
  QCHECK(BootAttempts(root) == kAttemptsOnRecord - 1);
  const json* given_back = trial("not_counted");
  QCHECK(given_back != nullptr && given_back->value("reason", "") == kGiveBackReason);
  QCHECK(given_back != nullptr && given_back->value(kBootAttemptsKey, kNoBootAttempts) == kAttemptsOnRecord - 1);
  return true;
}

// With no update root the start is the baseline alone, and the runtime is handed no swap capability: nothing was decided.
bool NoRootStartOffersNoSwapCapability() {
  const fs::path base = CaseRoot("no-root");
  g_records.clear();
  const std::string baseline = BaselineDir(base, "ok");
  us::Inputs in = InputsFor(fs::path(), baseline);
  in.root.clear();
  const us::Result result = StartCase(in);
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kBaseline);
  QCHECK(CapabilitiesSeenBy((fs::path(baseline) / kRuntimeFile).string()) == kNoCapability);
  return true;
}

// A link at the runtime's path is not followed (O_NOFOLLOW): its open fails, the failure is recorded, and the baseline
// loads.
bool LinkedRuntimeIsNotFollowed() {
  const fs::path base = CaseRoot("linked-runtime");
  g_records.clear();
  const fs::path root = base / "nevr";
  const fs::path target = base / "target.so";
  WriteAll(target, ReadAll(FixturePath("ok")));
  fs::create_directories(root / kUpdateDir);
  fs::create_symlink(target, root / kRuntimeFile);
  const us::Result result = StartCase(InputsFor(root, BaselineDir(base, "ok")));
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kBaseline);
  const Captured* absent = Find(kEventRuntimeLoad, kOutcomeAbsent);
  QCHECK(absent != nullptr && absent->Field("source") == kSourceUpdate);
  return true;
}

// A release that names the PC runtime's file, or a plugin beside the Quest runtime, is refused whole (the vocabulary of
// a Quest start is the runtime file alone): the engine discards it with reason bad_staged, the root keeps its runtime,
// and the start loads it.
bool ForeignMembersAreRefused() {
  const std::vector<std::vector<std::string>> releases = {{kPcRuntimeFile}, {kRuntimeFile, kPluginFile}};
  for (const std::vector<std::string>& paths : releases) {
    const fs::path base = CaseRoot("foreign-members");
    g_records.clear();
    const fs::path root = base / "nevr";
    SeedOwnedRuntime(root, "null_descriptor");
    const std::string bytes = ReadAll(FixturePath("ok"));
    json files = json::array();
    for (const std::string& path : paths) {
      WriteAll(root / kUpdateDir / kStagingDir / kVersionOne / path, bytes);
      files.push_back(json{{"path", path}, {"size", bytes.size()}, {"sha256", Digest(bytes)}});
    }
    WriteAll(root / kUpdateDir / kStagedJson, json{{"version", kVersionOne}, {"files", files}}.dump());
    const us::Result result = StartCase(InputsFor(root, BaselineDir(base, "ok")));
    const Captured* discard = Find(kEventSwap, kOutcomeDiscarded);
    QCHECK(discard != nullptr && discard->Field("reason") == kReasonBadStaged);
    QCHECK(Digest(ReadAll(root / kRuntimeFile)) == Digest(ReadAll(FixturePath("null_descriptor"))));
    QCHECK(result.origin == us::Origin::kBaseline);
  }
  return true;
}

// A swapped-in runtime whose entry is refused is put back in the same start: the previous release (the seeded one)
// loads from the root, and the failed one is not run again.
bool RefusedEntryRollsBackToThePreviousRelease() {
  const fs::path base = CaseRoot("rollback-previous");
  g_records.clear();
  const fs::path root = base / "nevr";
  SeedOwnedRuntime(root, "ok");
  StageRelease(root, kVersionTwo, "null_descriptor", false);
  const us::Result result = StartCase(InputsFor(root, BaselineDir(base, "ok")));
  QCHECK(!result.stop);
  QCHECK(result.outcome == rl::Outcome::kLoaded);
  QCHECK(result.origin == us::Origin::kUpdate);
  QCHECK(Digest(ReadAll(root / kRuntimeFile)) == Digest(ReadAll(FixturePath("ok"))));
  const Captured* load = Find(kEventRuntimeLoad, kOutcomeNullDescriptor);
  QCHECK(load != nullptr && load->Field("source") == kSourceUpdate);
  const Captured* rollback = Find(kEventRollback, kOutcomeRolledBack);
  QCHECK(rollback != nullptr && rollback->level == rl::kLogLevelWarning);
  return true;
}

// A pending release whose initialize fails: the process stops with the stop code, game_stop names the update, and the
// failure is marked in pending.json for the next start.
bool InitFailureStopsAndIsMarked() {
  const fs::path base = CaseRoot("init-failed");
  g_records.clear();
  const fs::path root = base / "nevr";
  SeedOwnedRuntime(root, "ok");
  StageRelease(root, kVersionTwo, "init_fail", false);
  const us::Result result = StartCase(InputsFor(root, BaselineDir(base, "ok")));
  QCHECK(result.stop);
  QCHECK(result.outcome == rl::Outcome::kInitFailed);
  QCHECK(us::kUpdateFailedExitCode == kUpdateFailedExitCodeExpected);
  const Captured* stop = Find(kEventStop);
  QCHECK(stop != nullptr && stop->Field("reason") == kReasonUpdateFailed);
  QCHECK(stop != nullptr && stop->level == rl::kLogLevelError);
  const json pending = json::parse(ReadAll(root / kUpdateDir / kPendingJson), nullptr, false);
  QCHECK(pending.is_object() && pending.value(kFailedReasonKey, std::string()) == kOutcomeInitFailed);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != kArgCount) {
    std::fprintf(stderr, "usage: %s <fixture-dir>\n", argv[0]);
    return kUsageExit;
  }
  g_dir = argv[1];
  BaselineWithoutUpdateFolder();
  StagedRuntimeIsSwappedInAndLoaded();
  FreshRootSwapsTheFirstRelease();
  FreshRootFailedReleaseLoadsTheBaseline();
  HostRefusalDoesNotJudgeTheRelease();
  NoRootStartOffersNoSwapCapability();
  MissingRootDirStillOffersSwapCapability();
  EngineStopOffersNoSwapCapability();
  CorruptStagingIsDiscarded();
  LinkedRuntimeIsNotFollowed();
  ForeignMembersAreRefused();
  RefusedEntryRollsBackToThePreviousRelease();
  InitFailureStopsAndIsMarked();
  ProductionShapeLoadsTheUpdate();
  HostRefusalFallsBackToTheBaselineAndCountsNoTry();
  UncountTryGivesBackOnlyThisStartsTry();
  const int failures = quest_test::Failures();
  std::printf("update_start_test: %s (%d failed checks)\n", failures == 0 ? "OK" : "FAILED", failures);
  return failures == 0 ? 0 : kFailedExit;
}
