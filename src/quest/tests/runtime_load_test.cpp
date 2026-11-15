// Host test for the shim's baseline start (src/quest/shim/runtime_load.h). The production LoadRuntimeFromFd and LoadBaseline
// map the fixture runtimes of runtime_load_fixture.cpp, one shared object per mode, and each start is checked for its outcome,
// for the context the runtime's initialize was handed, for the descriptor it took ownership of, and for the one record it wrote.
//
// Run: runtime_load_test <fixture-dir>
// The fixture directory is the integration group's build directory, where nevr_ht_so puts libnevr_fixture_<mode>.so.

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/quest_runtime_abi.h"
#include "quest/shim/runtime_load.h"
#include "quest/tests/test_check.h"

namespace {

using nevr_quest::runtime_load::HostInputs;
using nevr_quest::runtime_load::Outcome;

constexpr int kArgCount = 2;
constexpr int kUsageExit = 2;
constexpr int kFailedExit = 1;
constexpr mode_t kDirMode = 0755;
constexpr std::uint32_t kVariant = 3;
// The capability word the shim hands the runtime: bit 0, the staged swap (piece 15; the shim runs the update start before it loads).
constexpr std::uint64_t kSwapCapability = nevr_runtime_abi::kHostCapabilitySwap;
constexpr char kBuildId[] = "0123456789abcdef0123456789abcdef01234567";
constexpr char kShimVersion[] = "6.18.0-test";
constexpr char kBaselineName[] = "libnevr-runtime.so";
constexpr char kRecordTag[] = "NEVR.HOST";
constexpr char kRecordEvent[] = "runtime_load";
constexpr char kAbsentField[] = "<absent>";
constexpr char kFixtureMarker[] = "NEVR_FixtureLastContext";
constexpr char kFixtureCallsMarker[] = "NEVR_FixtureInitCalls";
constexpr char kFixtureOk[] = "ok";
constexpr char kFixtureNoEntry[] = "no_entry";
constexpr char kFixtureNullDescriptor[] = "null_descriptor";
constexpr char kFixtureBadMajor[] = "bad_major";
constexpr char kFixtureInitFail[] = "init_fail";
constexpr std::int32_t kFixtureInitFailStatus = 7;
constexpr char kNotElfText[] = "not an ELF image\n";

// The one record a start wrote, as the sink saw it.
struct Captured {
  std::uint32_t level = 0;
  std::string tag;
  std::string event;
  std::vector<std::pair<std::string, std::string>> fields;

  std::string Field(const char* key) const {
    for (const auto& field : fields) {
      if (field.first == key) return field.second;
    }
    return kAbsentField;
  }
};

std::vector<Captured> g_records;
std::string g_dir;
// Stands for the shim's own handle and for the libr15 base: any non-null address the test owns.
char g_shimAnchor = 0;

void CaptureSink(std::uint32_t level, const char* tag, const char* event, const char* message,
                 const NevrQuestLogField* fields, std::uint32_t count) {
  static_cast<void>(message);
  Captured record;
  record.level = level;
  record.tag = tag;
  record.event = event;
  for (std::uint32_t i = 0; i < count; ++i) record.fields.emplace_back(fields[i].key, fields[i].value);
  g_records.push_back(record);
}

HostInputs Inputs() {
  HostInputs host;
  host.shim_handle = &g_shimAnchor;
  host.libr15_base = &g_shimAnchor;
  host.libr15_build_id = kBuildId;
  host.update_root = nullptr;
  host.shim_version = kShimVersion;
  host.variant = kVariant;
  host.log = &CaptureSink;
  host.capabilities = kSwapCapability;
  return host;
}

std::string FixturePath(const char* mode) { return g_dir + "/libnevr_fixture_" + mode + ".so"; }

// Each start maps the fixture by its descriptor's /proc name, and glibc hands back an object already loaded under the same name.
// Descriptor numbers are reused once closed, so every start gets a number no earlier start used: a reused number would give a later
// fixture the earlier fixture's entry point and initialize.
constexpr int kFirstFixtureDescriptor = 100;
int g_nextFixtureDescriptor = kFirstFixtureDescriptor;

// LoadBaseline opens the baseline itself, so its descriptor is the lowest free number, which an earlier start may have used. Before a
// start that maps, this holds open every free number up to a new high-water mark (the held descriptors stay open for the rest of the
// run), so the start's own open gets a number no earlier start used.
int g_descriptorHighWater = 0;

void UseAFreshDescriptorNumber() {
  for (;;) {
    const int probe = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    QCHECK(probe >= 0);
    if (probe < 0) return;
    if (probe > g_descriptorHighWater) {
      g_descriptorHighWater = probe;
      ::close(probe);  // the start's open takes this number: it is above every number used before
      return;
    }
  }
}

int OpenFixture(const char* mode) {
  const int opened = ::open(FixturePath(mode).c_str(), O_RDONLY | O_CLOEXEC);
  if (opened < 0) return opened;
  const int unique = ::fcntl(opened, F_DUPFD_CLOEXEC, g_nextFixtureDescriptor++);
  ::close(opened);
  return unique;
}

// A handle to the fixture the start mapped: the same file maps to the same object, so dlopen by path returns its state.
void* FixtureHandle(const char* mode) { return dlopen(FixturePath(mode).c_str(), RTLD_NOW); }

const NevrQuestHostContext* FixtureContext(const char* mode) {
  void* handle = FixtureHandle(mode);
  if (handle == nullptr) return nullptr;
  using Getter = const NevrQuestHostContext* (*)();
  const auto get = reinterpret_cast<Getter>(dlsym(handle, kFixtureMarker));
  return get == nullptr ? nullptr : get();
}

std::uint32_t FixtureInitCalls(const char* mode) {
  void* handle = FixtureHandle(mode);
  if (handle == nullptr) return 0;
  using Counter = std::uint32_t (*)();
  const auto count = reinterpret_cast<Counter>(dlsym(handle, kFixtureCallsMarker));
  return count == nullptr ? 0 : count();
}

// The record of the last start: one record, its tag and event, the level, and the outcome and source it names.
void ExpectOneRecord(const char* outcome, std::uint32_t level) {
  QCHECK(g_records.size() == 1);
  if (g_records.size() != 1) return;
  const Captured& record = g_records.front();
  QCHECK(record.tag == kRecordTag);
  QCHECK(record.event == kRecordEvent);
  QCHECK(record.level == level);
  QCHECK(record.Field("outcome") == outcome);
  QCHECK(record.Field("source") == "baseline");
  QCHECK(record.Field("file") == kBaselineName);
  QCHECK(record.Field("verify") == "none");
}

bool DescriptorIsClosed(int fd) { return fcntl(fd, F_GETFD) == -1 && errno == EBADF; }

// A folder under the fixture directory, made if it is not there yet.
std::string Folder(const char* name) {
  const std::string path = g_dir + "/loader";
  ::mkdir(path.c_str(), kDirMode);
  const std::string folder = path + "/" + name;
  ::mkdir(folder.c_str(), kDirMode);
  ::unlink((folder + "/" + kBaselineName).c_str());
  return folder;
}

void CheckLoadedContext() {
  const NevrQuestHostContext* context = FixtureContext(kFixtureOk);
  QCHECK(context != nullptr);
  if (context == nullptr) return;
  QCHECK(context->header.struct_size == nevr_quest_runtime_abi::kHostContextV1Size);
  QCHECK(context->header.abi_major == nevr_quest_runtime_abi::kMajor);
  QCHECK(context->header.abi_minor == nevr_quest_runtime_abi::kMinor);
  QCHECK(context->shim_handle == static_cast<void*>(&g_shimAnchor));
  QCHECK(context->libr15_base == static_cast<const void*>(&g_shimAnchor));
  QCHECK(context->libr15_build_id != nullptr && std::strcmp(context->libr15_build_id, kBuildId) == 0);
  QCHECK(context->update_root == nullptr);
  QCHECK(context->shim_version != nullptr && std::strcmp(context->shim_version, kShimVersion) == 0);
  QCHECK(context->log == &CaptureSink);
  QCHECK(context->variant == kVariant);
  QCHECK(context->capabilities == kSwapCapability);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != kArgCount) {
    std::fprintf(stderr, "usage: runtime_load_test <fixture-dir>\n");
    return kUsageExit;
  }
  g_dir = argv[1];

  // 1. absent: a folder with no baseline in it. The open fails with ENOENT, and the record carries that errno.
  {
    const std::string folder = Folder("absent");
    errno = 0;
    QCHECK(nevr_quest::runtime_load::OpenBaselineIn(folder.c_str()) == -1);
    QCHECK(errno == ENOENT);
    g_records.clear();
    QCHECK(nevr_quest::runtime_load::LoadBaseline(folder.c_str(), Inputs()) == Outcome::kAbsent);
    ExpectOneRecord("absent", nevr_quest::runtime_load::kLogLevelError);
    if (g_records.size() == 1) QCHECK(g_records.front().Field("errno") == std::to_string(ENOENT));
  }

  // 2. loaded through the descriptor the caller passes, the production path a checked file takes later. The start owns the
  // descriptor (it is closed when the start returns) and the runtime's initialize is handed the context once.
  {
    const int fd = OpenFixture(kFixtureOk);
    QCHECK(fd >= 0);
    g_records.clear();
    const Outcome outcome = nevr_quest::runtime_load::LoadRuntimeFromFd(fd, kBaselineName, Inputs());
    QCHECK(DescriptorIsClosed(fd));  // checked first: the calls below may reuse the number
    QCHECK(outcome == Outcome::kLoaded);
    ExpectOneRecord("loaded", nevr_quest::runtime_load::kLogLevelInfo);
    QCHECK(FixtureInitCalls(kFixtureOk) == 1);
    CheckLoadedContext();
  }

  // 3. loaded through the baseline's own name: a folder whose libnevr-runtime.so is the fixture (a symbolic link, so the start
  // opens the fixture's file and maps it by the descriptor it holds).
  {
    const std::string folder = Folder("present");
    const std::string link = folder + "/" + kBaselineName;
    QCHECK(::symlink(FixturePath(kFixtureOk).c_str(), link.c_str()) == 0);
    g_records.clear();
    UseAFreshDescriptorNumber();
    QCHECK(nevr_quest::runtime_load::LoadBaseline(folder.c_str(), Inputs()) == Outcome::kLoaded);
    ExpectOneRecord("loaded", nevr_quest::runtime_load::kLogLevelInfo);
  }

  // 4. no entry: the file maps and has no NEVR_GetRuntimeApi export.
  {
    const int fd = OpenFixture(kFixtureNoEntry);
    QCHECK(fd >= 0);
    g_records.clear();
    QCHECK(nevr_quest::runtime_load::LoadRuntimeFromFd(fd, kBaselineName, Inputs()) == Outcome::kMissingEntry);
    ExpectOneRecord("missing_entry", nevr_quest::runtime_load::kLogLevelError);
    if (g_records.size() == 1) QCHECK(g_records.front().Field("reason") == "no_export");
  }

  // 5. null descriptor: NEVR_GetRuntimeApi returns null, so nothing is called through it.
  {
    const int fd = OpenFixture(kFixtureNullDescriptor);
    QCHECK(fd >= 0);
    g_records.clear();
    QCHECK(nevr_quest::runtime_load::LoadRuntimeFromFd(fd, kBaselineName, Inputs()) == Outcome::kNullDescriptor);
    ExpectOneRecord("null_descriptor", nevr_quest::runtime_load::kLogLevelError);
  }

  // 6. ABI rejected: the descriptor's abi_major is not the one the shim speaks, so initialize is never called.
  {
    const int fd = OpenFixture(kFixtureBadMajor);
    QCHECK(fd >= 0);
    const std::uint32_t callsBefore = FixtureInitCalls(kFixtureOk);
    g_records.clear();
    QCHECK(nevr_quest::runtime_load::LoadRuntimeFromFd(fd, kBaselineName, Inputs()) == Outcome::kAbiRejected);
    ExpectOneRecord("abi_rejected", nevr_quest::runtime_load::kLogLevelError);
    if (g_records.size() == 1) QCHECK(g_records.front().Field("reason") == "major_mismatch");
    QCHECK(FixtureInitCalls(kFixtureOk) == callsBefore);
  }

  // 7. init failed: initialize returns a nonzero status, which the record carries.
  {
    const int fd = OpenFixture(kFixtureInitFail);
    QCHECK(fd >= 0);
    g_records.clear();
    QCHECK(nevr_quest::runtime_load::LoadRuntimeFromFd(fd, kBaselineName, Inputs()) == Outcome::kInitFailed);
    ExpectOneRecord("init_failed", nevr_quest::runtime_load::kLogLevelError);
    if (g_records.size() == 1) {
      QCHECK(g_records.front().Field("initialize_status") == std::to_string(kFixtureInitFailStatus));
    }
  }

  // 8. load failed: the baseline is not an ELF image, so the dynamic linker refuses it and the record carries its message.
  {
    const std::string folder = Folder("not_elf");
    const std::string path = folder + "/" + kBaselineName;
    std::FILE* file = std::fopen(path.c_str(), "wb");
    QCHECK(file != nullptr);
    if (file != nullptr) {
      std::fwrite(kNotElfText, 1, sizeof kNotElfText - 1, file);
      std::fclose(file);
    }
    g_records.clear();
    UseAFreshDescriptorNumber();
    QCHECK(nevr_quest::runtime_load::LoadBaseline(folder.c_str(), Inputs()) == Outcome::kLoadFailed);
    ExpectOneRecord("load_failed", nevr_quest::runtime_load::kLogLevelError);
    if (g_records.size() == 1) QCHECK(g_records.front().Field("detail") != kAbsentField);
  }

  // 9. host context refused: without the shim's handle the runtime is not called at all, and the descriptor is still closed.
  {
    const int fd = OpenFixture(kFixtureOk);
    QCHECK(fd >= 0);
    const std::uint32_t callsBefore = FixtureInitCalls(kFixtureOk);
    HostInputs host = Inputs();
    host.shim_handle = nullptr;
    g_records.clear();
    const Outcome outcome = nevr_quest::runtime_load::LoadRuntimeFromFd(fd, kBaselineName, host);
    QCHECK(DescriptorIsClosed(fd));  // checked first: the calls below may reuse the number
    QCHECK(outcome == Outcome::kHostContextRefused);
    ExpectOneRecord("host_context_refused", nevr_quest::runtime_load::kLogLevelError);
    if (g_records.size() == 1) QCHECK(g_records.front().Field("reason") == "no_shim_handle");
    QCHECK(FixtureInitCalls(kFixtureOk) == callsBefore);
  }

  // 10. the start beside the shim: this test binary's own folder holds no baseline, so the start is absent, and the record says
  // the open failed. This checks the folder the start takes from the shim's address.
  {
    g_records.clear();
    QCHECK(nevr_quest::runtime_load::StartBaselineBesideShim(&g_shimAnchor, Inputs()) == Outcome::kAbsent);
    ExpectOneRecord("absent", nevr_quest::runtime_load::kLogLevelError);
    if (g_records.size() == 1) QCHECK(g_records.front().Field("reason") == "open_failed");
  }

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "runtime_load_test: %d check(s) failed\n", quest_test::Failures());
    return kFailedExit;
  }
  std::printf("runtime_load_test: all checks pass\n");
  return 0;
}
