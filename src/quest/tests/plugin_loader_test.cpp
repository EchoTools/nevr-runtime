// Host test for the Quest plugin loader (docs/adr/0019-quest-plugin-loader.md, slice 1; src/quest/plugin/).
//
// The plan, the file and build-ID checks and the record log are driven directly. The loader is driven against real shared
// objects (the fixture plugin, src/quest/tests/plugin_fixture.cpp, built into one lib<variant>.so per way a plugin can be
// right or wrong and loaded with dlopen), so the exports, the context the plugin receives, and the order of "build ID
// checked, then init" are the production code's, not a model of it. Nothing here proves Bionic behaviour: where the
// constructor-time dlopen, the library directory form and the timing are concerned, the ADR's predictions (P1, P2, P4, P8,
// P9, P10, P12, P13) wait for a headset.
//
// Run: plugin_loader_test <fixture-dir> [name-filter]

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/data/sha256.h"
#include "quest/patch/libr15_patch_identity.h"
#include "quest/patch/libr15_patch_table.h"
#include "quest/plugin/plugin_identity.h"
#include "quest/plugin/input_seam_hook.h"
#include "quest/plugin/plugin_loader.h"
#include "quest/plugin/plugin_platform.h"
#include "quest/plugin/plugin_record_log.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/mini_test.h"
#include "quest/tests/plugin_fixture_seen.h"

using namespace nevr_quest::plugin;

namespace {

std::string g_dir;  // where the fixture plugins were built

std::vector<std::pair<int, std::string>>& LogcatWrites() {
  static std::vector<std::pair<int, std::string>> writes;
  return writes;
}

#if defined(__aarch64__)
constexpr std::uint16_t kHostMachine = kMachineAarch64;
constexpr std::uint16_t kOtherMachine = kMachineX86_64;
#else
constexpr std::uint16_t kHostMachine = kMachineX86_64;
constexpr std::uint16_t kOtherMachine = kMachineAarch64;
#endif

// The running libr15 build the fake platform reports: the second ID the fixture lists.
const char* const kGameBuild = "2222222222222222222222222222222222222222";
const NevrQuestHostApi kFakeQuestApi{sizeof(NevrQuestHostApi), nullptr};
// The main level the fake platform reports (its name is set beside it: the loader copies both, it does not look one up).
constexpr std::uint64_t kArenaLevel = 0x42670f2bed45703cULL;

std::vector<std::uint8_t> Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class FakePlatform final : public Platform {
 public:
  ~FakePlatform() override {
    for (auto& entry : handles) ::dlclose(entry.second);  // the next test sees a fresh library
  }

  bool ReadFile(const std::string& file, std::vector<std::uint8_t>* bytes) override {
    ++reads;
    if (throwOnRead == file) throw std::runtime_error("read");
    const auto in_memory = memory.find(file);
    if (in_memory != memory.end()) {
      *bytes = in_memory->second;
      return true;
    }
    if (!readFromDisk) return false;
    *bytes = Slurp(g_dir + "/" + file);
    return !bytes->empty();
  }
  void* Open(const std::string& file) override {
    opened.push_back(file);
    const auto alias = openAs.find(file);
    void* handle = ::dlopen((g_dir + "/" + (alias != openAs.end() ? alias->second : file)).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle != nullptr) handles[file] = handle;
    return handle;
  }
  void* Symbol(void* handle, const char* name) override {
    if (!throwOnSymbol.empty() && throwOnSymbol == name) throw std::runtime_error("symbol");
    return ::dlsym(handle, name);
  }
  std::string GameBuildId() override {
    ++buildIdReads;
    return gameBuild;
  }
  Libr15PatchState Libr15Patch() override { return patch; }
  std::uintptr_t GameBase() override { return base; }
  void Emit(const LoadRecord& record) override { records.push_back(record); }
  void EmitPlan(std::size_t entries, std::size_t early, std::size_t normal, std::size_t refused,
                std::size_t disabled) override {
    plans.push_back({entries, early, normal, refused, disabled});
  }
  void PluginLine(std::uint32_t level, const std::string& line) override { lines.push_back({level, line}); }
  const NevrQuestHostApi* QuestHostApi() override { return questApi; }
  MainLevel CurrentMainLevel() override { return level; }

  // Unloads the object for real (the next test sees a fresh library) after keeping a copy of what it recorded.
  void Close(void* handle) override {
    for (auto it = handles.begin(); it != handles.end(); ++it) {
      if (it->second != handle) continue;
      if (const auto* seen = static_cast<const FxSeen*>(::dlsym(handle, "fx_seen"))) closedSeen[it->first] = *seen;
      closed.push_back(it->first);
      ::dlclose(handle);
      handles.erase(it);
      return;
    }
  }

  // What the plugin recorded: live while its object is mapped, the copy taken at Close afterwards, null if it never was.
  const FxSeen* Seen(const std::string& file) const {
    const auto it = handles.find(file);
    if (it != handles.end()) return static_cast<const FxSeen*>(::dlsym(it->second, "fx_seen"));
    const auto gone = closedSeen.find(file);
    return gone == closedSeen.end() ? nullptr : &gone->second;
  }
  const LoadRecord* Record(const std::string& name) const {
    const LoadRecord* found = nullptr;
    for (const LoadRecord& r : records) {
      if (r.name == name) {
        if (found != nullptr) return nullptr;  // two records for one entry: the caller's CHECK on the count says so
        found = &r;
      }
    }
    return found;
  }
  std::size_t RecordsFor(const std::string& name) const {
    std::size_t n = 0;
    for (const LoadRecord& r : records) n += r.name == name ? 1 : 0;
    return n;
  }

  std::string gameBuild = kGameBuild;
  // What Attach recorded. The default is our verified patched file (the live note is the ID verification covered), which is
  // the only libr15 a plugin loads on (`libr15_not_verified`); a test that means the stock or an unverified file says so.
  Libr15PatchState patch = {true, kGameBuild};
  std::size_t buildIdReads = 0;
  std::map<std::string, std::string> openAs;       // file served from `memory` -> the fixture object that is mapped for it
  std::uintptr_t base = 0x7000000000;
  const NevrQuestHostApi* questApi = &kFakeQuestApi;
  MainLevel level;  // the main level the game reports (#1106); none until a case sets it
  bool readFromDisk = true;
  std::string throwOnRead;
  std::string throwOnSymbol;
  std::map<std::string, std::vector<std::uint8_t>> memory;
  std::map<std::string, void*> handles;
  std::vector<std::string> opened;
  std::vector<std::string> closed;
  std::map<std::string, FxSeen> closedSeen;
  std::size_t reads = 0;
  std::vector<LoadRecord> records;
  struct Plan {
    std::size_t entries, early, normal, refused, disabled;
  };
  std::vector<Plan> plans;
  std::vector<std::pair<std::uint32_t, std::string>> lines;
};

Options HostOptions() {
  Options options;
  options.machine = kHostMachine;
  return options;
}

struct Env {
  explicit Env(Options options = HostOptions()) : loader(new Loader(platform, options)) {}
  FakePlatform platform;
  std::unique_ptr<Loader> loader;
};

PluginLoadItem Item(const std::string& name, const std::string& file, bool early = false, bool enabled = true,
                    const std::string& args = "{}") {
  PluginLoadItem item;
  item.name = name;
  item.file = file;
  item.early = early;
  item.enabled = enabled;
  item.args_json = args;
  return item;
}

// ---- the plan -------------------------------------------------------------------------------------------------------

TEST(PlanAppliesTheQuestDefaultFile) {
  const auto plan = BuildPlan({Item("alpha", "alpha.dll"), Item("beta", "libcustom.so")}, true);
  CHECK_EQ(plan.size(), std::size_t{2});
  CHECK_EQ(plan[0].item.file, std::string("libalpha.so"));
  CHECK_EQ(plan[1].item.file, std::string("libcustom.so"));
  CHECK(plan[0].pending && plan[1].pending);
}

TEST(PlanSettlesADisabledEntryWithoutPendingIt) {
  const auto plan = BuildPlan({Item("alpha", "alpha.dll", false, false)}, true);
  CHECK(!plan[0].pending);
  CHECK(plan[0].outcome == Outcome::kDisabled);
  CHECK_EQ(plan[0].reasonToken, std::string(reason::kDisabled));
}

TEST(PlanRefusesAFileWithAPathSeparator) {
  for (const char* file : {"../libx.so", "sub/libx.so", "sub\\libx.so", "/data/libx.so"}) {
    const auto plan = BuildPlan({Item("x", file)}, true);
    CHECK(!plan[0].pending);
    CHECK(plan[0].outcome == Outcome::kRefused);
    CHECK_EQ(plan[0].reasonToken, std::string(reason::kFileOutsideDirectory));
  }
  CHECK(BuildPlan({Item("x", "libx.so")}, true)[0].pending);
}

TEST(PlanRefusesNulEmptyDotAndDotDotFiles) {
  const std::string nul("libx.so\0y", 9);
  for (const std::string& file : {nul, std::string("\0", 1), std::string(""), std::string("."), std::string("..")}) {
    const auto plan = BuildPlan({Item("x", file)}, true);
    CHECK(!plan[0].pending);
    CHECK(plan[0].outcome == Outcome::kRefused);
    CHECK_EQ(plan[0].reasonToken, std::string(reason::kFileOutsideDirectory));
  }
  // "...", ".x" and "x." are file names, not directory references.
  for (const char* file : {"...", ".libx.so", "libx."}) CHECK(BuildPlan({Item("x", file)}, true)[0].pending);
  // Two entries cannot name one OS path by way of a NUL: the path would end at it, so the second would init the plugin again.
  const auto twice = BuildPlan({Item("a", "libsame.so"), Item("b", std::string("libsame.so\0tail", 15))}, true);
  CHECK(twice[0].pending);
  CHECK(!twice[1].pending);
  CHECK_EQ(twice[1].reasonToken, std::string(reason::kFileOutsideDirectory));
}

TEST(PlanRefusesASecondEntryNamingTheSameFile) {
  const auto plan = BuildPlan({Item("a", "libsame.so"), Item("b", "libsame.so")}, true);
  CHECK(plan[0].pending);
  CHECK(!plan[1].pending);
  CHECK_EQ(plan[1].reasonToken, std::string(reason::kDuplicateFile));
}

TEST(PlanDoesNotCountAFileTheEarlierEntryNeverLoads) {
  // A disabled earlier entry never loads, so it does not claim the file.
  const auto disabled = BuildPlan({Item("a", "libsame.so", false, false), Item("b", "libsame.so")}, true);
  CHECK(disabled[1].pending);
  // Neither does an earlier entry the plan refused.
  const auto refused = BuildPlan({Item("a", "libsame.so", true), Item("b", "libsame.so")}, false);
  CHECK_EQ(refused[0].reasonToken, std::string(reason::kEarlyUnsupported));
  CHECK(refused[1].pending);
}

TEST(PlanRefusesEarlyEntriesOnlyWhenEarlyIsUnsupported) {
  const auto unsupported = BuildPlan({Item("e", "libe.so", true), Item("n", "libn.so")}, false);
  CHECK(!unsupported[0].pending);
  CHECK(unsupported[0].outcome == Outcome::kRefused);
  CHECK_EQ(unsupported[0].reasonToken, std::string(reason::kEarlyUnsupported));
  CHECK(unsupported[1].pending);
  const auto supported = BuildPlan({Item("e", "libe.so", true)}, true);
  CHECK(supported[0].pending);
  CHECK(supported[0].phase == PluginPhase::Early);
}

TEST(PlanKeepsListOrder) {
  const auto plan = BuildPlan({Item("c", "libc.so"), Item("a", "liba.so"), Item("b", "libb.so")}, true);
  CHECK_EQ(plan[0].item.name, std::string("c"));
  CHECK_EQ(plan[1].item.name, std::string("a"));
  CHECK_EQ(plan[2].item.name, std::string("b"));
}

TEST(ArgKeysNamesTheKeysAndNeverTheValues) {
  CHECK_EQ(ArgKeys(R"({"b":"secret-b","a":"secret-a"})"), std::string("a,b"));
  CHECK_EQ(ArgKeys("{}"), std::string(""));
  CHECK_EQ(ArgKeys("not json"), std::string(""));
  CHECK(ArgKeys(R"({"token":"hunter2"})").find("hunter2") == std::string::npos);
}

// ---- the file and the build IDs ------------------------------------------------------------------------------------

// A 64-bit little-endian ELF header with `phnum` program headers after it.
std::vector<std::uint8_t> ElfHeader(std::uint16_t type, std::uint16_t machine, std::uint16_t phnum = 0) {
  std::vector<std::uint8_t> b(64 + phnum * 56, 0);
  const unsigned char magic[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
  std::memcpy(b.data(), magic, sizeof(magic));
  b[16] = static_cast<std::uint8_t>(type);
  b[17] = static_cast<std::uint8_t>(type >> 8);
  b[18] = static_cast<std::uint8_t>(machine);
  b[19] = static_cast<std::uint8_t>(machine >> 8);
  b[32] = 64;  // e_phoff
  b[54] = 56;  // e_phentsize
  b[56] = static_cast<std::uint8_t>(phnum);
  return b;
}

TEST(ImageAcceptsASharedObjectForTheMachine) {
  const auto b = ElfHeader(3, kMachineAarch64);
  CHECK(IsSharedObjectFor(b.data(), b.size(), kMachineAarch64));
}

TEST(ImageRefusesWhatIsNotAnAarch64SharedObject) {
  const auto x86 = ElfHeader(3, kMachineX86_64);
  CHECK(!IsSharedObjectFor(x86.data(), x86.size(), kMachineAarch64));
  const auto exe = ElfHeader(2, kMachineAarch64);  // ET_EXEC
  CHECK(!IsSharedObjectFor(exe.data(), exe.size(), kMachineAarch64));
  auto big = ElfHeader(3, kMachineAarch64);
  big[5] = 2;  // big-endian
  CHECK(!IsSharedObjectFor(big.data(), big.size(), kMachineAarch64));
  auto narrow = ElfHeader(3, kMachineAarch64);
  narrow[4] = 1;  // 32-bit
  CHECK(!IsSharedObjectFor(narrow.data(), narrow.size(), kMachineAarch64));
  auto notElf = ElfHeader(3, kMachineAarch64);
  notElf[3] = 'G';  // "\x7fELG": everything else a valid AArch64 shared object header
  CHECK(!IsSharedObjectFor(notElf.data(), notElf.size(), kMachineAarch64));
  const std::vector<std::uint8_t> pe = {'M', 'Z', 0x90, 0, 3, 0, 0, 0};  // a PC plugin DLL
  CHECK(!IsSharedObjectFor(pe.data(), pe.size(), kMachineAarch64));
  const auto truncated = ElfHeader(3, kMachineAarch64);
  CHECK(!IsSharedObjectFor(truncated.data(), 20, kMachineAarch64));
  CHECK(!IsSharedObjectFor(nullptr, 0, kMachineAarch64));
}

// An ELF with one PT_NOTE segment holding one note.
std::vector<std::uint8_t> ElfWithNote(std::uint32_t type, const char* name4, std::uint32_t descSize, std::uint32_t declaredDesc) {
  std::vector<std::uint8_t> b = ElfHeader(3, kMachineAarch64, 1);
  const std::uint64_t noteOffset = b.size();
  const std::uint64_t noteSize = 12 + 4 + ((descSize + 3) & ~3u);
  auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
  auto put64 = [&](std::size_t at, std::uint64_t v) { std::memcpy(b.data() + at, &v, 8); };
  b.resize(noteOffset + noteSize, 0);
  put32(64, 4);                 // p_type = PT_NOTE
  put64(64 + 8, noteOffset);    // p_offset
  put64(64 + 32, noteSize);     // p_filesz
  put32(noteOffset, 4);         // namesz
  put32(noteOffset + 4, declaredDesc);
  put32(noteOffset + 8, type);
  std::memcpy(b.data() + noteOffset + 12, name4, 4);
  for (std::uint32_t i = 0; i < descSize; ++i) b[noteOffset + 16 + i] = static_cast<std::uint8_t>(0xa0 + i);
  return b;
}

TEST(ImageReadsTheGnuBuildIdNote) {
  const auto b = ElfWithNote(3, "GNU", 4, 4);
  CHECK_EQ(BuildIdOf(b.data(), b.size()), std::string("a0a1a2a3"));
}

// An ELF with one PT_NOTE segment holding one note owned by "NEVR" (namesz 5, padded to 8), the shape
// NEVR_DECLARE_SUPPORTED_BUILD_IDS (plugin_interface.h) emits.
std::vector<std::uint8_t> ElfWithNevrNote(std::uint32_t type, const std::string& text, std::uint32_t declaredDesc,
                                          std::uint32_t namesz = 5) {
  std::vector<std::uint8_t> b = ElfHeader(3, kMachineAarch64, 1);
  const std::uint64_t noteOffset = b.size();
  const std::uint64_t noteSize = 12 + 8 + ((text.size() + 3) & ~std::size_t{3});
  auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
  auto put64 = [&](std::size_t at, std::uint64_t v) { std::memcpy(b.data() + at, &v, 8); };
  b.resize(noteOffset + noteSize, 0);
  put32(64, 4);                 // p_type = PT_NOTE
  put64(64 + 8, noteOffset);    // p_offset
  put64(64 + 32, noteSize);     // p_filesz
  put32(noteOffset, namesz);
  put32(noteOffset + 4, declaredDesc);
  put32(noteOffset + 8, type);
  std::memcpy(b.data() + noteOffset + 12, "NEVR", 5);
  std::memcpy(b.data() + noteOffset + 20, text.data(), text.size());
  return b;
}

TEST(ImageReadsTheDeclaredBuildIdsFromTheFileWithoutMappingIt) {
  const auto b = ElfWithNevrNote(1, "aaaa bbbb", 9);
  const auto ids = SupportedBuildIdsOf(b.data(), b.size());
  CHECK(ids.has_value());
  if (ids.has_value()) CHECK_EQ(*ids, std::string("aaaa bbbb"));
  // A fixture compiled with the macro carries the same note, found in the real linked object.
  const std::vector<std::uint8_t> real = Slurp(g_dir + "/libfx_ok.so");
  const auto declared = SupportedBuildIdsOf(real.data(), real.size());
  CHECK(declared.has_value());
  if (declared.has_value()) {
    CHECK_EQ(*declared, std::string("1111111111111111111111111111111111111111 2222222222222222222222222222222222222222"));
  }
  const std::vector<std::uint8_t> undeclared = Slurp(g_dir + "/libfx_noids.so");
  CHECK(!SupportedBuildIdsOf(undeclared.data(), undeclared.size()).has_value());
}

TEST(ImageIgnoresANoteThatIsNotTheNevrBuildIdList) {
  const auto wrongType = ElfWithNevrNote(2, "aaaa", 4);
  CHECK(!SupportedBuildIdsOf(wrongType.data(), wrongType.size()).has_value());
  auto wrongOwner = ElfWithNevrNote(1, "aaaa", 4);
  wrongOwner[64 + 56 + 12] = 'X';  // "XEVR"
  CHECK(!SupportedBuildIdsOf(wrongOwner.data(), wrongOwner.size()).has_value());
  const auto longerOwner = ElfWithNevrNote(1, "aaaa", 4, 8);  // "NEVR\0\0\0\0": the owner is "NEVR" exactly
  CHECK(!SupportedBuildIdsOf(longerOwner.data(), longerOwner.size()).has_value());
  const auto gnu = ElfWithNote(3, "GNU", 4, 4);  // the GNU build ID is not the plugin's list
  CHECK(!SupportedBuildIdsOf(gnu.data(), gnu.size()).has_value());
  const auto none = ElfHeader(3, kMachineAarch64);
  CHECK(!SupportedBuildIdsOf(none.data(), none.size()).has_value());
  CHECK(!SupportedBuildIdsOf(nullptr, 0).has_value());
}

TEST(ImageReadsNoDeclaredListPastTheFile) {
  const auto lying = ElfWithNevrNote(1, "aaaa", 0xffffffffu);
  CHECK(!SupportedBuildIdsOf(lying.data(), lying.size()).has_value());
  auto cut = ElfWithNevrNote(1, "aaaaaaaa", 8);
  cut.resize(cut.size() - 6);
  CHECK(!SupportedBuildIdsOf(cut.data(), cut.size()).has_value());
  const auto huge = ElfWithNevrNote(1, std::string(70 * 1024, 'a'), 70 * 1024);  // beyond the 64 KiB bound
  CHECK(!SupportedBuildIdsOf(huge.data(), huge.size()).has_value());
}

TEST(ImageGivesNoBuildIdWhenTheNoteIsNotOne) {
  const auto wrongType = ElfWithNote(1, "GNU", 4, 4);
  CHECK_EQ(BuildIdOf(wrongType.data(), wrongType.size()), std::string(""));
  const auto wrongName = ElfWithNote(3, "XYZ", 4, 4);
  CHECK_EQ(BuildIdOf(wrongName.data(), wrongName.size()), std::string(""));
  const auto none = ElfHeader(3, kMachineAarch64);
  CHECK_EQ(BuildIdOf(none.data(), none.size()), std::string(""));
}

TEST(ImageReadsNothingPastTheFile) {
  // The note claims 4 GiB of descriptor; the segment holds 8 bytes. Must neither read past the buffer nor crash.
  const auto lying = ElfWithNote(3, "GNU", 4, 0xffffffffu);
  CHECK_EQ(BuildIdOf(lying.data(), lying.size()), std::string(""));
  auto cut = ElfWithNote(3, "GNU", 4, 4);
  cut.resize(cut.size() - 6);  // the segment now ends past the file
  CHECK_EQ(BuildIdOf(cut.data(), cut.size()), std::string(""));
  auto phoff = ElfHeader(3, kMachineAarch64, 1);
  phoff[32] = 0xff;
  phoff[39] = 0x7f;  // e_phoff far past the file
  CHECK_EQ(BuildIdOf(phoff.data(), phoff.size()), std::string(""));
}

TEST(BuildIdVerdictMatchesTheWholeIdOnly) {
  const std::string running = "2222222222222222222222222222222222222222";
  CHECK(DecideBuildId("1111111111111111111111111111111111111111 2222222222222222222222222222222222222222", running) ==
        BuildIdVerdict::kSupported);
  CHECK(DecideBuildId("1111,2222222222222222222222222222222222222222", running) == BuildIdVerdict::kSupported);
  CHECK(DecideBuildId("1111\n2222222222222222222222222222222222222222\n", running) == BuildIdVerdict::kSupported);
  CHECK(DecideBuildId("3333333333333333333333333333333333333333", running) == BuildIdVerdict::kUnsupported);
  CHECK(DecideBuildId("", running) == BuildIdVerdict::kUnsupported);
  CHECK(DecideBuildId("   ", running) == BuildIdVerdict::kUnsupported);
  // A prefix of the running build, or a list entry that is a prefix of it, is not the build.
  CHECK(DecideBuildId("22222222", running) == BuildIdVerdict::kUnsupported);
  CHECK(DecideBuildId("22222222222222222222222222222222222222223", running) == BuildIdVerdict::kUnsupported);
}

TEST(BuildIdVerdictNamesAnAbsentExportAndAnUnknownGame) {
  CHECK(DecideBuildId(nullptr, "2222") == BuildIdVerdict::kUndeclared);
  CHECK(DecideBuildId("2222", "") == BuildIdVerdict::kUnknownGame);
  CHECK(DecideBuildId(nullptr, "") == BuildIdVerdict::kUndeclared);
}

const std::string& StockId() {
  static const std::string id = nevr_quest::patch::kLibR15StockBuildId;
  return id;
}

// ---- the loader, against real shared objects -----------------------------------------------------------------------

TEST(LoaderLoadsANoopPluginWithOneRecord) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.records.size(), std::size_t{1});
  const LoadRecord* r = env.platform.Record("ok");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kLoaded);
  CHECK_EQ(r->reason, std::string(""));
  CHECK(r->phase == PluginPhase::Normal);
  CHECK_EQ(r->file, std::string("libfx_ok.so"));
  CHECK_EQ(r->gameBuildId, std::string(kGameBuild));
  CHECK_EQ(r->pluginViewBuildId, StockId());  // our verified patched file: the stock ID is the plugin identity
  CHECK(r->libr15Patched);
  const std::vector<std::uint8_t> bytes = Slurp(g_dir + "/libfx_ok.so");
  CHECK(!bytes.empty());
  CHECK_EQ(r->sha256, nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()));
  CHECK_EQ(r->sha256.size(), std::size_t{64});
  CHECK_EQ(r->pluginBuildId, BuildIdOf(bytes.data(), bytes.size()));
  CHECK_EQ(r->pluginBuildId.size(), std::size_t{40});  // the fixtures are linked with --build-id=sha1
}

// A packaged entry carries the hash the repack wrote; the loader checks it on the bytes it read, before the machine check and
// before dlopen (docs/adr/0019-quest-plugin-loader.md, section 1 trust).
PlannedItem Packaged(const std::string& name, const std::string& file, const std::string& expectedSha256) {
  PlannedItem planned;
  planned.item = Item(name, file);
  planned.source = Source::kPackaged;
  planned.expectedSha256 = expectedSha256;
  return planned;
}

TEST(LoaderRefusesAPackagedFileWhoseHashIsNotTheManifests) {
  Env env;
  env.loader->Plan({Packaged("ok", "libfx_ok.so", std::string(64, '0'))});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("ok");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kRefused);
  CHECK_EQ(r->reason, std::string(reason::kManifestHashMismatch));
  CHECK_EQ(r->source, std::string("packaged"));
  CHECK(env.platform.opened.empty());  // refused before dlopen: nothing was mapped
  // The record names both hashes, so the log says what the manifest expected next to what the file is.
  CHECK_EQ(r->expectedSha256, std::string(64, '0'));
  CHECK_EQ(r->sha256.size(), std::size_t{64});
  CHECK(r->sha256 != r->expectedSha256);
}

// The hash gate follows the file, not the entry: a second config entry that names a packaged file (the first one disabled, or
// the file spelled in another case) is a config-only item and still has the manifest's hash checked before dlopen.
TEST(LoaderChecksTheManifestHashOnASecondConfigEntryForTheSameFile) {
  PackagedPlugin packaged;
  packaged.name = "ok";
  packaged.file = "libfx_ok.so";
  packaged.sha256 = std::string(64, '0');  // not the fixture's hash
  Env env;
  env.loader->Plan(MergePackaged({packaged}, {Item("ok", "libfx_ok.so", false, false), Item("ok2", "libfx_ok.so")}));
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* second = env.platform.Record("ok2");
  CHECK(second != nullptr);
  if (second == nullptr) return;
  CHECK(second->outcome == Outcome::kRefused);
  CHECK_EQ(second->reason, std::string(reason::kManifestHashMismatch));
  CHECK_EQ(second->source, std::string("config"));
  CHECK(env.platform.opened.empty());  // nothing was mapped
}

TEST(LoaderLoadsAPackagedFileWhoseHashMatchesTheManifest) {
  // The fixture is built for this host, so with a matching hash the plugin loads: the hash is a gate, not a filter.
  Env env;
  const std::vector<std::uint8_t> file = Slurp(g_dir + "/libfx_ok.so");
  CHECK(!file.empty());
  if (file.empty()) return;
  const std::string sha = nevr_quest::data::Sha256Hex(file.data(), file.size());
  env.loader->Plan({Packaged("ok", "libfx_ok.so", sha)});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("ok");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kLoaded);
  CHECK_EQ(r->source, std::string("packaged"));
  CHECK_EQ(r->sha256, sha);
}

// config.yaml rejected as a whole: the packaged plugin still loads (docs/adr/0019 section 2), and its record says the operator's
// file did not load, so a disabled entry that was lost is visible on the line. A run whose file was usable says 0.
TEST(LoaderSaysOnThePackagedRecordThatTheConfigFileWasRejected) {
  Options rejected = HostOptions();
  rejected.configRejected = true;
  Env env(rejected);
  const std::vector<std::uint8_t> file = Slurp(g_dir + "/libfx_ok.so");
  CHECK(!file.empty());
  if (file.empty()) return;
  const std::string sha = nevr_quest::data::Sha256Hex(file.data(), file.size());
  env.loader->Plan({Packaged("ok", "libfx_ok.so", sha)});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("ok");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kLoaded);
  CHECK(r->configRejected);

  Env usable;
  usable.loader->Plan({Packaged("ok", "libfx_ok.so", sha)});
  usable.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* u = usable.platform.Record("ok");
  CHECK(u != nullptr);
  if (u == nullptr) return;
  CHECK(u->outcome == Outcome::kLoaded);
  CHECK(!u->configRejected);
}

TEST(LoaderGivesThePluginTheQuestContext) {
  Env env;
  env.platform.level = {kArenaLevel, "mpl_arena_a"};
  env.loader->Plan({Item("ok", "libfx_ok.so", false, true, R"({"k":"v"})")});
  env.loader->RunPhase(PluginPhase::Normal);
  const FxSeen* seen = env.platform.Seen("libfx_ok.so");
  CHECK(seen != nullptr);
  if (seen == nullptr) return;
  CHECK_EQ(seen->init_count, 1u);
  CHECK_EQ(seen->flags, static_cast<std::uint32_t>(NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST));
  CHECK_EQ(seen->ctx_size, static_cast<std::uint32_t>(sizeof(NvrGameContext)));
  CHECK_EQ(seen->base_addr, env.platform.base);
  CHECK(seen->net_game == nullptr);
  CHECK_EQ(seen->game_state, 0u);
  CHECK_EQ(std::string(seen->plugin_id), std::string("ok"));
  CHECK_EQ(std::string(seen->args), std::string(R"({"k":"v"})"));
  CHECK_EQ(seen->covers_game_build_id, 1);
  CHECK_EQ(std::string(seen->game_build_id), StockId());  // the plugin identity, not the live note
  // The platform's Quest host services (#1202) reach the plugin as they are.
  CHECK_EQ(seen->covers_quest, 1);
  CHECK(seen->quest == env.platform.questApi);
  // The main level the platform reports when the context is built (#1106): its symbol, and the name the table gives it.
  CHECK_EQ(seen->level_symbol, std::uint64_t{kArenaLevel});
  CHECK(seen->level_name != nullptr);
  if (seen->level_name != nullptr) CHECK_EQ(std::string(seen->level_name), std::string("mpl_arena_a"));
}

TEST(LoaderRoutesThePluginsLogLineThroughTheHostFormat) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.lines.size(), std::size_t{1});
  if (env.platform.lines.empty()) return;
  CHECK_EQ(env.platform.lines[0].first, static_cast<std::uint32_t>(NEVR_LOG_INFO));
  CHECK_EQ(env.platform.lines[0].second, std::string("[NEVR.PLUGIN] plugin=ok fixture init k=v"));
}

TEST(LoaderEndsEveryEntryWithExactlyOneRecord) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so"), Item("off", "liboff.so", false, false),
                    Item("out", "../libfx_ok.so"), Item("noids", "libfx_noids.so"),
                    Item("dup", "libfx_ok.so"), Item("late", "libfx_second.so", true)});
  env.loader->RunPhase(PluginPhase::Normal);
  env.loader->Finish(reason::kNoInstallMoment);
  CHECK_EQ(env.platform.records.size(), std::size_t{6});
  for (const char* name : {"ok", "off", "out", "noids", "dup", "late"}) CHECK_EQ(env.platform.RecordsFor(name), std::size_t{1});
  // A second Finish, or another pass, adds nothing: the entries are settled.
  env.loader->Finish(reason::kNoInstallMoment);
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.records.size(), std::size_t{6});
  CHECK(env.platform.Record("off")->outcome == Outcome::kDisabled);
  CHECK(env.platform.Record("out")->outcome == Outcome::kRefused);
  CHECK(env.platform.Record("dup")->outcome == Outcome::kRefused);
}

TEST(LoaderWritesTheSettledEntriesAtPlanTimeAndLoadsNothing) {
  Env env;
  env.loader->Plan({Item("off", "liboff.so", false, false), Item("out", "sub/libx.so"), Item("ok", "libfx_ok.so")});
  CHECK_EQ(env.platform.records.size(), std::size_t{2});
  CHECK(env.platform.opened.empty());
  CHECK_EQ(env.platform.reads, std::size_t{0});
  CHECK_EQ(env.platform.plans.size(), std::size_t{1});
  const auto& plan = env.platform.plans[0];
  CHECK_EQ(plan.entries, std::size_t{3});
  CHECK_EQ(plan.refused, std::size_t{1});
  CHECK_EQ(plan.disabled, std::size_t{1});
  CHECK_EQ(plan.normal, std::size_t{1});
  CHECK_EQ(plan.early, std::size_t{0});
  CHECK_EQ(env.platform.Record("off")->sha256, std::string("none"));
}

TEST(LoaderRunsTheEarlyPassForEarlyEntriesOnly) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so"), Item("second", "libfx_second.so", true)});
  CHECK_EQ(env.loader->PendingFor(PluginPhase::Early), std::size_t{1});
  env.loader->RunPhase(PluginPhase::Early);
  CHECK_EQ(env.platform.records.size(), std::size_t{1});
  CHECK(env.platform.Record("second")->phase == PluginPhase::Early);
  CHECK(env.platform.Record("second")->outcome == Outcome::kLoaded);
  CHECK(env.platform.Record("ok") == nullptr);
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.records.size(), std::size_t{2});
  CHECK(env.platform.Record("ok")->phase == PluginPhase::Normal);
  // The early entry is not loaded a second time by the normal pass.
  CHECK_EQ(env.platform.RecordsFor("second"), std::size_t{1});
}

TEST(LoaderTakesEarlyEntriesInTheNormalPassWhenNoEarlyPassRan) {
  Env env;
  env.loader->Plan({Item("second", "libfx_second.so", true)});
  CHECK_EQ(env.loader->PendingFor(PluginPhase::Normal), std::size_t{1});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("second")->outcome == Outcome::kLoaded);
}

TEST(LoaderRefusesAnEarlyEntryWhenEarlyIsUnsupported) {
  Options options = HostOptions();
  options.earlySupported = false;
  Env env(options);
  env.loader->Plan({Item("second", "libfx_second.so", true)});
  env.loader->RunPhase(PluginPhase::Early);
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.records.size(), std::size_t{1});
  CHECK(env.platform.Record("second")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("second")->reason, std::string(reason::kEarlyUnsupported));
  CHECK(env.platform.opened.empty());
}

TEST(LoaderRefusesAPluginThatDoesNotListTheRunningBuild) {
  Env env;
  env.loader->Plan({Item("otherids", "libfx_otherids.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("otherids");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kRefused);
  CHECK_EQ(r->reason, std::string(reason::kBuildIdUnsupported));
  // Refused from the file's bytes: the object was never mapped, so none of its code (constructors included) ran.
  CHECK(env.platform.opened.empty());
  CHECK(env.platform.Seen("libfx_otherids.so") == nullptr);
}

TEST(LoaderRefusesAPluginWithoutTheBuildIdNote) {
  Env env;
  env.loader->Plan({Item("noids", "libfx_noids.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("noids")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("noids")->reason, std::string(reason::kBuildIdsUndeclared));
  CHECK(env.platform.opened.empty());
}

TEST(LoaderRefusesEveryPluginWhenTheRunningBuildCannotBeRead) {
  Env env;
  env.platform.gameBuild = "";
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("ok")->reason, std::string(reason::kGameBuildIdUnavailable));
  CHECK_EQ(env.platform.Record("ok")->gameBuildId, std::string("none"));
  CHECK(env.platform.opened.empty());
}

TEST(LoaderReadsTheRunningBuildAgainOnALaterPass) {
  // The early pass runs before libr15 is mapped, so its build cannot be read; the normal pass runs after, and must ask again.
  Env env;
  env.platform.gameBuild = "";
  env.loader->Plan({Item("early", "libfx_second.so", true), Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Early);
  CHECK(env.platform.Record("early")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("early")->reason, std::string(reason::kGameBuildIdUnavailable));
  env.platform.gameBuild = kGameBuild;
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kLoaded);
  CHECK_EQ(env.platform.Record("ok")->gameBuildId, std::string(kGameBuild));
}

// ---- the plugin identity (ADR 0021 section 12, test 10) ------------------------------------------------------------

// The live note of our patched libr15: what the running file carries once the patcher has written it.
const std::string& DerivedId() {
  static const std::string id = nevr_quest::patch::PatchedBuildId();
  return id;
}
const char* const kThirdId = "3333333333333333333333333333333333333333";

// Serves libfx_ok.so's bytes as `file` with the declared-IDs note rewritten to `ids` (the note keeps its size: the text is
// padded with spaces), and maps libfx_ok.so itself when the loader opens it. The declared list is what the loader decides
// from, so this is a plugin that declares exactly `ids`.
void ServeDeclaring(FakePlatform& platform, const std::string& file, const std::string& ids) {
  std::vector<std::uint8_t> bytes = Slurp(g_dir + "/libfx_ok.so");
  const std::string original =
      "1111111111111111111111111111111111111111 2222222222222222222222222222222222222222";
  const auto at = std::search(bytes.begin(), bytes.end(), original.begin(), original.end());
  CHECK(at != bytes.end());
  if (at == bytes.end()) return;
  std::string text = ids;
  CHECK(text.size() <= original.size());
  text.resize(original.size(), ' ');
  std::copy(text.begin(), text.end(), at);
  platform.memory[file] = bytes;
  platform.openAs[file] = "libfx_ok.so";
}

// Our patched libr15 as the loader meets it: the live note is the derived ID and Attach recorded it verified.
void RunsPatched(FakePlatform& platform) {
  platform.gameBuild = DerivedId();
  platform.patch.verified = true;
  platform.patch.verifiedBuildId = DerivedId();
}

TEST(PluginViewIsTheStockIdOnlyForTheVerifiedPatchedFile) {
  const std::string derived = DerivedId();
  const PluginView patched = ResolvePluginView(derived.c_str(), true, derived.c_str());
  CHECK(patched.patched);
  CHECK_EQ(std::string(patched.buildId), StockId());
  // Anything missing leaves the live note as the identity.
  const PluginView unverified = ResolvePluginView(derived.c_str(), false, derived.c_str());
  CHECK(!unverified.patched);
  CHECK_EQ(std::string(unverified.buildId), derived);
  const PluginView moved = ResolvePluginView(kThirdId, true, derived.c_str());  // verified, but the note now read is another ID
  CHECK(!moved.patched);
  CHECK_EQ(std::string(moved.buildId), std::string(kThirdId));
  const PluginView other = ResolvePluginView(derived.c_str(), true, kThirdId);  // the verification covered another ID
  CHECK(!other.patched);
  CHECK_EQ(std::string(other.buildId), derived);
  const PluginView noId = ResolvePluginView(derived.c_str(), true, nullptr);
  CHECK(!noId.patched);
  const PluginView stock = ResolvePluginView(StockId().c_str(), false, nullptr);
  CHECK(!stock.patched);
  CHECK_EQ(std::string(stock.buildId), StockId());
  // An unreadable live note is never mapped, whatever was verified.
  CHECK(!ResolvePluginView("", true, "").patched);
  CHECK(!ResolvePluginView(nullptr, true, nullptr).patched);
  CHECK_EQ(std::string(ResolvePluginView(nullptr, false, nullptr).buildId), std::string(""));
}

TEST(BuildIdVerdictForAViewAcceptsTheViewOrTheLiveNote) {
  const std::string derived = DerivedId();
  const PluginView patched = ResolvePluginView(derived.c_str(), true, derived.c_str());
  CHECK(DecideBuildIdForView(StockId().c_str(), derived, patched) == BuildIdVerdict::kSupported);
  CHECK(DecideBuildIdForView(derived.c_str(), derived, patched) == BuildIdVerdict::kSupported);
  CHECK(DecideBuildIdForView(kThirdId, derived, patched) == BuildIdVerdict::kUnsupported);
  CHECK(DecideBuildIdForView(nullptr, derived, patched) == BuildIdVerdict::kUndeclared);
  // Not patched: the view is the live note, and only the live note decides.
  const PluginView live = ResolvePluginView(derived.c_str(), false, nullptr);
  CHECK(DecideBuildIdForView(derived.c_str(), derived, live) == BuildIdVerdict::kSupported);
  CHECK(DecideBuildIdForView(StockId().c_str(), derived, live) == BuildIdVerdict::kUnsupported);
  const PluginView unknown = ResolvePluginView("", false, nullptr);
  CHECK(DecideBuildIdForView(StockId().c_str(), "", unknown) == BuildIdVerdict::kUnknownGame);
}

TEST(LoaderLoadsAStockIdPluginOnTheVerifiedPatchedFile) {
  Env env;
  RunsPatched(env.platform);
  ServeDeclaring(env.platform, "libstock.so", StockId());
  env.loader->Plan({Item("stock", "libstock.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("stock");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kLoaded);
  CHECK_EQ(r->reason, std::string(""));
  CHECK_EQ(r->gameBuildId, DerivedId());       // the log still says which file ran
  CHECK_EQ(r->pluginViewBuildId, StockId());   // and which ID decided
  CHECK(r->libr15Patched);
  const FxSeen* seen = env.platform.Seen("libstock.so");
  CHECK(seen != nullptr);
  if (seen == nullptr) return;
  CHECK_EQ(seen->init_count, 1u);
  CHECK_EQ(seen->covers_game_build_id, 1);
  CHECK_EQ(std::string(seen->game_build_id), StockId());  // a plugin that copies it into its targets works on both files
}

TEST(LoaderRefusesAStockIdPluginWhenTheDerivedNoteWasNotVerified) {
  Env env;
  env.platform.gameBuild = DerivedId();  // the note alone: Attach did not record `verified`
  ServeDeclaring(env.platform, "libstock.so", StockId());
  env.loader->Plan({Item("stock", "libstock.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("stock");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kRefused);
  CHECK_EQ(r->reason, std::string(reason::kBuildIdUnsupported));
  CHECK_EQ(r->gameBuildId, DerivedId());
  CHECK_EQ(r->pluginViewBuildId, DerivedId());
  CHECK(!r->libr15Patched);
  CHECK(env.platform.opened.empty());
}

TEST(LoaderRefusesAStockIdPluginWhenTheStateCarriesTheIdButNotTheVerifiedFlag) {
  Env env;
  RunsPatched(env.platform);
  env.platform.patch.verified = false;  // the ID is there and equals the note; the process did not record `verified`
  ServeDeclaring(env.platform, "libstock.so", StockId());
  env.loader->Plan({Item("stock", "libstock.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("stock");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kRefused);
  CHECK_EQ(r->reason, std::string(reason::kBuildIdUnsupported));
  CHECK_EQ(r->pluginViewBuildId, DerivedId());
  CHECK(!r->libr15Patched);
  CHECK(env.platform.opened.empty());
}

TEST(LoaderRefusesAStockIdPluginWhenTheNoteReadNowIsAnotherId) {
  Env env;
  RunsPatched(env.platform);
  env.platform.gameBuild = kThirdId;  // verified earlier, but the note the loader reads at the decision is another ID
  ServeDeclaring(env.platform, "libstock.so", StockId());
  env.loader->Plan({Item("stock", "libstock.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("stock");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kRefused);
  CHECK_EQ(r->reason, std::string(reason::kBuildIdUnsupported));
  CHECK_EQ(r->gameBuildId, std::string(kThirdId));
  CHECK_EQ(r->pluginViewBuildId, std::string(kThirdId));
  CHECK(!r->libr15Patched);
  CHECK(env.platform.opened.empty());
}

TEST(LoaderRefusesAStockIdPluginWhenTheVerificationCoveredAnotherId) {
  Env env;
  env.platform.gameBuild = DerivedId();
  env.platform.patch.verified = true;
  env.platform.patch.verifiedBuildId = kThirdId;
  ServeDeclaring(env.platform, "libstock.so", StockId());
  env.loader->Plan({Item("stock", "libstock.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("stock")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("stock")->reason, std::string(reason::kBuildIdUnsupported));
  CHECK(!env.platform.Record("stock")->libr15Patched);
}

TEST(LoaderRefusesAStockIdPluginOnAThirdLibr15) {
  Env env;
  env.platform.gameBuild = kThirdId;
  ServeDeclaring(env.platform, "libstock.so", StockId());
  env.loader->Plan({Item("stock", "libstock.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("stock");
  CHECK(r->outcome == Outcome::kRefused);
  CHECK_EQ(r->reason, std::string(reason::kBuildIdUnsupported));
  CHECK_EQ(r->gameBuildId, std::string(kThirdId));
  CHECK(!r->libr15Patched);
  CHECK(env.platform.opened.empty());
}

TEST(LoaderDecidesASecondPluginOnTheNoteReadForIt) {
  Env env;
  RunsPatched(env.platform);
  ServeDeclaring(env.platform, "libfirst.so", StockId());
  ServeDeclaring(env.platform, "libsecond.so", StockId());
  env.loader->Plan({Item("first", "libfirst.so", true), Item("second", "libsecond.so")});
  env.loader->RunPhase(PluginPhase::Early);  // loads "first" on the patched file
  const LoadRecord* first = env.platform.Record("first");
  CHECK(first != nullptr && first->outcome == Outcome::kLoaded);
  env.platform.gameBuild = kThirdId;         // the note read for the next decision
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* second = env.platform.Record("second");
  CHECK(second != nullptr);
  if (second != nullptr) {
    CHECK(second->outcome == Outcome::kRefused);
    CHECK_EQ(second->reason, std::string(reason::kBuildIdUnsupported));
    CHECK_EQ(second->gameBuildId, std::string(kThirdId));
  }
  // The first plugin's context still names the stock ID: the string it was handed is not the loader's current note.
  const FxSeen* seen = env.platform.Seen("libfirst.so");
  CHECK(seen != nullptr);
  if (seen != nullptr) CHECK_EQ(std::string(seen->game_build_id), StockId());
}

TEST(LoaderLoadsADerivedIdPluginOnThePatchedFileAndRefusesItOnAStockOne) {
  {
    Env env;
    RunsPatched(env.platform);
    ServeDeclaring(env.platform, "libderived.so", DerivedId());
    env.loader->Plan({Item("derived", "libderived.so")});
    env.loader->RunPhase(PluginPhase::Normal);
    const LoadRecord* r = env.platform.Record("derived");
    CHECK(r->outcome == Outcome::kLoaded);
    CHECK(r->libr15Patched);
    CHECK_EQ(r->gameBuildId, DerivedId());
    CHECK_EQ(r->pluginViewBuildId, StockId());
  }
  {
    Env env;
    env.platform.gameBuild = StockId();  // the stock file: no patch recorded
    ServeDeclaring(env.platform, "libderived.so", DerivedId());
    env.loader->Plan({Item("derived", "libderived.so")});
    env.loader->RunPhase(PluginPhase::Normal);
    const LoadRecord* r = env.platform.Record("derived");
    CHECK(r->outcome == Outcome::kRefused);
    CHECK_EQ(r->reason, std::string(reason::kBuildIdUnsupported));
    CHECK(!r->libr15Patched);
    CHECK(env.platform.opened.empty());
  }
}

// The falsifier of the verified-patch rule: a stock libr15 whose live note is the stock ID, with nothing verified, must not
// load a plugin that declares that ID (the plugin's addresses would be taken on a file that was never patched).
TEST(LoaderRefusesAStockIdPluginOnTheStockFileWithoutTheMapping) {
  Env env;
  env.platform.gameBuild = StockId();
  env.platform.patch = {};
  ServeDeclaring(env.platform, "libstock.so", StockId());
  env.loader->Plan({Item("stock", "libstock.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("stock");
  CHECK(r != nullptr);
  if (r == nullptr) return;
  CHECK(r->outcome == Outcome::kRefused);
  CHECK_EQ(r->reason, std::string(reason::kLibR15NotVerified));
  CHECK_EQ(r->gameBuildId, StockId());
  CHECK_EQ(r->pluginViewBuildId, StockId());
  CHECK(!r->libr15Patched);
  CHECK(env.platform.opened.empty());  // refused before dlopen: nothing was mapped
}

// The token is the same in both passes: a plugin whose list names the running build is refused on an unverified libr15 before
// the early pass (the early entry) and in the normal pass (the normal entry), and a plugin the verdict refuses keeps its reason.
TEST(LoaderRefusesAMatchingPluginOnAnUnverifiedLibR15InBothPhases) {
  Env env;
  env.platform.patch = {};  // the live note is kGameBuild, which libfx_second.so and libfx_ok.so list, and nothing is verified
  env.loader->Plan({Item("early", "libfx_second.so", true), Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Early);
  CHECK(env.platform.Record("early")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("early")->reason, std::string(reason::kLibR15NotVerified));
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("ok")->reason, std::string(reason::kLibR15NotVerified));
  CHECK_EQ(env.platform.Record("ok")->gameBuildId, std::string(kGameBuild));
  CHECK(env.platform.opened.empty());
}

TEST(LoaderKeepsTheVerdictReasonForAPluginTheListRefuses) {
  Env env;
  env.platform.patch = {};  // unverified: a plugin the list does not name still reports the verdict, not the verification
  env.loader->Plan({Item("otherids", "libfx_otherids.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.Record("otherids")->reason, std::string(reason::kBuildIdUnsupported));
}

TEST(LoaderGivesAnUnreadableLiveNoteNoPluginView) {
  Env env;
  env.platform.gameBuild = "";
  env.platform.patch.verified = true;  // verified, but there is no note to compare: no mapping, no view
  env.platform.patch.verifiedBuildId = DerivedId();
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.Record("ok")->reason, std::string(reason::kGameBuildIdUnavailable));
  CHECK_EQ(env.platform.Record("ok")->pluginViewBuildId, std::string("none"));
  CHECK(!env.platform.Record("ok")->libr15Patched);
}

TEST(LoaderRefusesAFileThatIsNotForThisMachineWithoutMappingIt) {
  Options options = HostOptions();
  options.machine = kOtherMachine;
  Env env(options);
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("ok")->reason, std::string(reason::kMachineType));
  CHECK(env.platform.opened.empty());
  CHECK_EQ(env.platform.Record("ok")->sha256.size(), std::size_t{64});  // read and hashed, never mapped
}

TEST(LoaderRefusesAPcPluginDll) {
  Env env;
  env.platform.memory["libpc.so"] = {'M', 'Z', 0x90, 0, 3, 0, 0, 0, 0xff, 0xff};
  env.loader->Plan({Item("pc", "libpc.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("pc")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("pc")->reason, std::string(reason::kMachineType));
  CHECK(env.platform.opened.empty());
}

TEST(LoaderRefusesAnEmptyFile) {
  Env env;
  env.platform.memory["libempty.so"] = {};
  env.loader->Plan({Item("empty", "libempty.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.Record("empty")->reason, std::string(reason::kMachineType));
}

TEST(LoaderDoesNotMapAFileItCannotRead) {
  // The library directory is not a directory (an APK-mapped path): the bytes cannot be checked, so nothing is mapped.
  Env env;
  env.platform.readFromDisk = false;
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("ok");
  CHECK(r->outcome == Outcome::kFailed);
  CHECK_EQ(r->reason, std::string(reason::kFileUnreadable));
  CHECK_EQ(r->sha256, std::string("unavailable"));
  CHECK_EQ(r->pluginBuildId, std::string("unavailable"));
  CHECK(env.platform.opened.empty());
}

TEST(LoaderReportsAMissingFileAsUnreadable) {
  Env env;
  env.loader->Plan({Item("gone", "libfx_missing.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("gone")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("gone")->reason, std::string(reason::kFileUnreadable));
  CHECK(env.platform.opened.empty());
}

TEST(LoaderReportsACheckedFileThatDoesNotMapAsADlopenFailure) {
  Env env;
  env.platform.memory["libfx_ghost.so"] = Slurp(g_dir + "/libfx_ok.so");  // passes every static check, but is not on disk
  env.loader->Plan({Item("ghost", "libfx_ghost.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ghost")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("ghost")->reason, std::string(reason::kDlopenFailed));
  CHECK_EQ(env.platform.opened.size(), std::size_t{1});
  CHECK(env.platform.closed.empty());  // nothing was mapped, so nothing to close
}

TEST(LoaderFailsAPluginWithoutGetInfo) {
  Env env;
  env.loader->Plan({Item("noinfo", "libfx_noinfo.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("noinfo")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("noinfo")->reason, std::string(reason::kNoPluginInfo));
  CHECK_EQ(env.platform.Seen("libfx_noinfo.so")->init_count, 0u);
  CHECK_EQ(env.platform.closed.size(), std::size_t{1});  // mapped, then refused: unmapped again
  CHECK_EQ(env.platform.closed[0], std::string("libfx_noinfo.so"));
}

TEST(LoaderFailsAPluginWhoseInitFailsAndContinues) {
  Env env;
  env.loader->Plan({Item("initfail", "libfx_initfail.so"), Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("initfail")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("initfail")->reason, std::string(reason::kInitFailed));
  CHECK_EQ(env.platform.Seen("libfx_initfail.so")->shutdown_count, 1u);  // the documented lifecycle
  CHECK(env.platform.Record("ok")->outcome == Outcome::kLoaded);
  CHECK_EQ(env.platform.closed.size(), std::size_t{1});  // the failed plugin is unmapped after its Shutdown; the loaded one stays
  CHECK_EQ(env.platform.closed[0], std::string("libfx_initfail.so"));
  CHECK(env.platform.handles.count("libfx_ok.so") == 1);
}

TEST(LoaderUnmapsAPluginWhenTheLoaderThrowsAfterMappingIt) {
  Env env;
  env.platform.throwOnSymbol = "NvrPluginGetInfo";
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("ok")->reason, std::string(reason::kException));
  CHECK_EQ(env.platform.closed.size(), std::size_t{1});
  CHECK_EQ(env.platform.RecordsFor("ok"), std::size_t{1});
}

TEST(LoaderKeepsALoadedPluginMapped) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kLoaded);
  CHECK(env.platform.closed.empty());
}

TEST(LoaderKeepsGoingWhenARequiredPluginFails) {
  Env env;
  PluginLoadItem required = Item("initfail", "libfx_initfail.so");
  required.required = true;
  env.loader->Plan({required, Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("initfail")->required);
  CHECK(env.platform.Record("initfail")->outcome == Outcome::kFailed);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kLoaded);
}

TEST(LoaderCallsTheV3InitWhenThePluginHasNoInitEx) {
  Env env;
  env.loader->Plan({Item("legacy", "libfx_legacy.so", false, true, R"({"k":"v"})")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("legacy")->outcome == Outcome::kLoaded);
  CHECK_EQ(env.platform.Seen("libfx_legacy.so")->init_count, 1u);
  CHECK_EQ(std::string(env.platform.Seen("libfx_legacy.so")->args), std::string(""));  // a v3 plugin gets no args
}

TEST(LoaderRecordsTheArgumentKeysAndNeverTheValues) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so", false, true, R"({"token":"hunter2","region":"eu"})")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("ok");
  CHECK_EQ(r->argKeys, std::string("region,token"));
  for (const LoadRecord& record : env.platform.records) {
    CHECK(record.argKeys.find("hunter2") == std::string::npos);
  }
}

TEST(LoaderOffersTheQueryApiToALaterPlugin) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so"), Item("second", "libfx_second.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const FxSeen* second = env.platform.Seen("libfx_second.so");
  CHECK_EQ(second->plugin_count_at_init, 1);
  CHECK_EQ(std::string(second->first_plugin_name), std::string("ok"));
  CHECK_EQ(env.platform.Seen("libfx_ok.so")->plugin_count_at_init, 0);
}

TEST(LoaderFinishEndsPendingEntriesAsNotStarted) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so"), Item("second", "libfx_second.so")});
  env.loader->Finish(reason::kGameExitedFirst);
  CHECK_EQ(env.platform.records.size(), std::size_t{2});
  for (const LoadRecord& r : env.platform.records) {
    CHECK(r.outcome == Outcome::kNotStarted);
    CHECK_EQ(r.reason, std::string(reason::kGameExitedFirst));
  }
  CHECK(env.platform.opened.empty());
  CHECK_EQ(env.loader->PendingFor(PluginPhase::Normal), std::size_t{0});
}

TEST(LoaderContainsAnExceptionFromThePlatform) {
  Env env;
  env.platform.throwOnRead = "libfx_ok.so";
  env.loader->Plan({Item("ok", "libfx_ok.so"), Item("second", "libfx_second.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("ok")->reason, std::string(reason::kException));
  CHECK(env.platform.Record("second")->outcome == Outcome::kLoaded);
}

TEST(LoaderPlansOnce) {
  Env env;
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->Plan({Item("second", "libfx_second.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK_EQ(env.platform.records.size(), std::size_t{1});
  CHECK(env.platform.Record("ok") != nullptr);
}

// ---- the log ------------------------------------------------------------------------------------------------------

std::vector<std::string>& Captured() {
  static std::vector<std::string> lines;
  return lines;
}
void CaptureLine(sentinel::LogLevel, const char* line) { Captured().push_back(line); }

struct CaptureScope {
  CaptureScope() : previous(sentinel::SetLogSink(&CaptureLine)) { Captured().clear(); }
  ~CaptureScope() { sentinel::SetLogSink(previous); }
  sentinel::LogSink previous;
};

TEST(RecordLogCarriesEveryFieldAndNoValue) {
  CaptureScope capture;
  LoadRecord record;
  record.name = "ok";
  record.file = "libok.so";
  record.sha256 = std::string(64, 'a');
  record.pluginBuildId = "bb";
  record.gameBuildId = "cc";
  record.pluginViewBuildId = "dd";
  record.libr15Patched = true;
  record.phase = PluginPhase::Early;
  record.outcome = Outcome::kRefused;
  record.reason = reason::kBuildIdUnsupported;
  record.required = true;
  record.argKeys = "region,token";
  record.source = "packaged";
  record.overridden = true;
  record.expectedSha256 = std::string(64, 'e');
  LogLoadRecord(record);
  CHECK_EQ(Captured().size(), std::size_t{1});
  if (Captured().empty()) return;
  const nlohmann::json line = nlohmann::json::parse(Captured()[0]);
  CHECK_EQ(line.value("event", ""), std::string("plugin_load"));
  CHECK_EQ(line.value("level", ""), std::string("warn"));
  CHECK_EQ(line.value("source", ""), std::string("packaged"));
  CHECK_EQ(line.value("overridden", -1), 1);
  CHECK_EQ(line.value("name", ""), std::string("ok"));
  CHECK_EQ(line.value("file", ""), std::string("libok.so"));
  CHECK_EQ(line.value("sha256", ""), std::string(64, 'a'));
  CHECK_EQ(line.value("expected_sha256", ""), std::string(64, 'e'));
  CHECK_EQ(line.value("plugin_build_id", ""), std::string("bb"));
  CHECK_EQ(line.value("game_build_id", ""), std::string("cc"));
  CHECK_EQ(line.value("plugin_view_build_id", ""), std::string("dd"));
  CHECK_EQ(line.value("libr15_patched", -1), 1);
  CHECK_EQ(line.value("phase", ""), std::string("early"));
  CHECK_EQ(line.value("outcome", ""), std::string("refused"));
  CHECK_EQ(line.value("reason", ""), std::string("build_id_unsupported"));
  CHECK_EQ(line.value("required", -1), 1);
  CHECK_EQ(line.value("arg_keys", ""), std::string("region,token"));
}

TEST(RecordLogNamesALoadedPluginOkAtInfo) {
  CaptureScope capture;
  LoadRecord record;
  record.name = "ok";
  record.file = "libok.so";
  record.outcome = Outcome::kLoaded;
  LogLoadRecord(record);
  const nlohmann::json line = nlohmann::json::parse(Captured().at(0));
  CHECK_EQ(line.value("level", ""), std::string("info"));
  CHECK_EQ(line.value("outcome", ""), std::string("loaded"));
  CHECK_EQ(line.value("reason", ""), std::string("ok"));
  // A record that never saw the game says so in fixed tokens: no view, not patched.
  CHECK_EQ(line.value("plugin_view_build_id", ""), std::string("none"));
  CHECK_EQ(line.value("libr15_patched", -1), 0);
}

TEST(PlanSummaryLogCarriesTheCounts) {
  CaptureScope capture;
  LogPlanSummary(6, 1, 2, 2, 1);
  const nlohmann::json line = nlohmann::json::parse(Captured().at(0));
  CHECK_EQ(line.value("event", ""), std::string("plugin_plan"));
  CHECK_EQ(line.value("entries", -1), 6);
  CHECK_EQ(line.value("early", -1), 1);
  CHECK_EQ(line.value("normal", -1), 2);
  CHECK_EQ(line.value("refused", -1), 2);
  CHECK_EQ(line.value("disabled", -1), 1);
}

// ---- the sentinel's platform ---------------------------------------------------------------------------------------

TEST(PlatformFindsTheDirectoryOfAMappingPath) {
  CHECK_EQ(DirectoryOf("/data/app/x/lib/arm64/libovrplatformloader.so"), std::string("/data/app/x/lib/arm64"));
  CHECK_EQ(DirectoryOf("base.apk!/lib/arm64-v8a/libx.so"), std::string("base.apk!/lib/arm64-v8a"));
  CHECK_EQ(DirectoryOf("libx.so"), std::string(""));
  CHECK_EQ(DirectoryOf(nullptr), std::string(""));
}

TEST(PlatformMapsPluginLogLevelsAndDropsDebugOutsideDevelopment) {
  using nevr_quest::LogLevel;
  CHECK(!SinkLevel(NEVR_LOG_DEBUG, false).has_value());
  CHECK(SinkLevel(NEVR_LOG_DEBUG, true) == LogLevel::kInfo);
  CHECK(SinkLevel(NEVR_LOG_INFO, false) == LogLevel::kInfo);
  CHECK(SinkLevel(NEVR_LOG_WARNING, false) == LogLevel::kWarn);
  CHECK(SinkLevel(NEVR_LOG_ERROR, false) == LogLevel::kError);
}

TEST(PlatformReadsARegularFileAndRefusesALinkAndADirectory) {
  char tmpl[] = "/tmp/nevr-plugin-platform-XXXXXX";
  const char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return;
  const std::string root = dir;
  {
    std::ofstream(root + "/libreal.so", std::ios::binary) << "bytes";
  }
  CHECK_EQ(::symlink((root + "/libreal.so").c_str(), (root + "/liblink.so").c_str()), 0);
  CHECK_EQ(::mkdir((root + "/libdir.so").c_str(), 0700), 0);
  CHECK_EQ(::mkfifo((root + "/libfifo.so").c_str(), 0600), 0);  // opens and reads as an empty file, but is not one
  SentinelPlatform platform(root, false);
  std::vector<std::uint8_t> bytes;
  CHECK(platform.ReadFile("libreal.so", &bytes));
  CHECK_EQ(bytes.size(), std::size_t{5});
  CHECK(!platform.ReadFile("liblink.so", &bytes));
  CHECK(!platform.ReadFile("libdir.so", &bytes));
  CHECK(!platform.ReadFile("libfifo.so", &bytes));
  CHECK(!platform.ReadFile("libmissing.so", &bytes));
  SentinelPlatform nowhere("", false);
  CHECK(!nowhere.ReadFile("libreal.so", &bytes));
  ::unlink((root + "/libreal.so").c_str());
  ::unlink((root + "/liblink.so").c_str());
  ::rmdir((root + "/libdir.so").c_str());
  ::unlink((root + "/libfifo.so").c_str());
  ::rmdir(root.c_str());
}

// A file that is not there is "absent"; a file that is there and cannot be used is "unreadable" with the reason. The sentinel logs
// the two differently (plugin_manifest status=absent / unreadable), so a manifest the loader could not read is not reported as
// a build with no packaged plugin.
TEST(PlatformSaysAbsentForAMissingFileAndUnreadableWithAReasonForTheRest) {
  char tmpl[] = "/tmp/nevr-plugin-outcome-XXXXXX";
  const char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return;
  const std::string root = dir;
  {
    std::ofstream(root + "/libreal.so", std::ios::binary) << "bytes";
  }
  CHECK_EQ(::symlink((root + "/libreal.so").c_str(), (root + "/liblink.so").c_str()), 0);
  CHECK_EQ(::mkdir((root + "/libdir.so").c_str(), 0700), 0);
  {
    std::ofstream big(root + "/libbig.so", std::ios::binary);
    big << "x";
  }
  CHECK_EQ(::truncate((root + "/libbig.so").c_str(), 65 * 1024 * 1024), 0);  // sparse: one byte over 64 MiB
  SentinelPlatform platform(root, false);
  std::vector<std::uint8_t> bytes;
  const char* why = nullptr;
  CHECK(platform.ReadFileOutcome("libreal.so", &bytes, &why) == ReadOutcome::kRead);
  CHECK_EQ(std::string(why), std::string(""));
  CHECK(platform.ReadFileOutcome("libmissing.so", &bytes, &why) == ReadOutcome::kAbsent);
  CHECK_EQ(std::string(why), std::string(""));
  CHECK(platform.ReadFileOutcome("liblink.so", &bytes, &why) == ReadOutcome::kUnreadable);
  CHECK_EQ(std::string(why), std::string("symlink"));
  CHECK(platform.ReadFileOutcome("libdir.so", &bytes, &why) == ReadOutcome::kUnreadable);
  CHECK_EQ(std::string(why), std::string("not_regular_file"));
  CHECK(platform.ReadFileOutcome("libbig.so", &bytes, &why) == ReadOutcome::kUnreadable);
  CHECK_EQ(std::string(why), std::string("too_large"));
  SentinelPlatform nowhere("", false);
  CHECK(nowhere.ReadFileOutcome("libreal.so", &bytes, &why) == ReadOutcome::kUnreadable);
  CHECK_EQ(std::string(why), std::string("no_library_dir"));
  ::unlink((root + "/libreal.so").c_str());
  ::unlink((root + "/liblink.so").c_str());
  ::unlink((root + "/libbig.so").c_str());
  ::rmdir((root + "/libdir.so").c_str());
  ::rmdir(root.c_str());
}

TEST(PlatformOpensTheDirectoryQualifiedPathAndNeverTheBareName) {
  SentinelPlatform platform(g_dir, false);
  void* const handle = platform.Open("libfx_ok.so");  // only in g_dir: a bare-name dlopen would search elsewhere and fail
  CHECK(handle != nullptr);
  if (handle != nullptr) {
    CHECK(platform.Symbol(handle, "NvrPluginGetInfo") != nullptr);
    ::dlclose(handle);
  }
  // A name the linker resolves from the system, absent from the library directory, must not load.
  CHECK(platform.Open("libc.so.6") == nullptr);
  CHECK(platform.Open("libmissing.so") == nullptr);
  SentinelPlatform nowhere("", false);
  CHECK(nowhere.Open("libc.so.6") == nullptr);
  CHECK(nowhere.Open("libfx_ok.so") == nullptr);
}

TEST(PlatformCloseUnmapsWhatOpenMapped) {
  const std::string path = g_dir + "/libfx_second.so";
  CHECK(::dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);  // not mapped yet: an earlier test unmapped it
  SentinelPlatform platform(g_dir, false);
  void* const handle = platform.Open("libfx_second.so");
  CHECK(handle != nullptr);
  void* const again = ::dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
  CHECK(again != nullptr);
  if (again != nullptr) ::dlclose(again);
  platform.Close(handle);
  CHECK(::dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
  platform.Close(nullptr);  // nothing to close is not an error
}

TEST(PlatformWritesAPluginLineToTheSentinelLog) {
  LogcatWrites().clear();
  SentinelPlatform platform("", false);
  platform.PluginLine(NEVR_LOG_INFO, "[NEVR.PLUGIN] plugin=ok hello");
  platform.PluginLine(NEVR_LOG_DEBUG, "[NEVR.PLUGIN] plugin=ok chatter");
  bool hello = false, chatter = false;
  for (const auto& write : LogcatWrites()) {
    hello = hello || write.second.find("plugin=ok hello") != std::string::npos;
    chatter = chatter || write.second.find("chatter") != std::string::npos;
  }
  CHECK(hello);
  CHECK(!chatter);
}

TEST(PlatformReadsThePatchStateFromItsSourceAndFailsClosedWithout) {
  SentinelPlatform platform("", false);
  const Libr15PatchSource previous = SetLibr15PatchSource(nullptr);
  CHECK(!platform.Libr15Patch().verified);  // no source: not verified
  CHECK_EQ(platform.Libr15Patch().verifiedBuildId, std::string(""));
  CHECK(SetLibr15PatchSource(+[]() { return Libr15PatchState{true, std::string("abcd")}; }) == nullptr);
  CHECK(platform.Libr15Patch().verified);
  CHECK_EQ(platform.Libr15Patch().verifiedBuildId, std::string("abcd"));
  CHECK(SetLibr15PatchSource(previous) != nullptr);
}

TEST(PlatformOffersTheInputSeamOfItsKindOfBuild) {
  SentinelPlatform dev("", true);
  SentinelPlatform release("", false);
  CHECK(dev.QuestHostApi() == nevr_quest::input_seam::HostApi(true));
  CHECK(release.QuestHostApi() == nevr_quest::input_seam::HostApi(false));
  CHECK(dev.QuestHostApi() != release.QuestHostApi());
}

TEST(PlatformSeesNoGameOnTheHost) {
  SentinelPlatform platform("", false);
  CHECK_EQ(platform.GameBuildId(), std::string(""));  // no libr15.so is mapped in this process
  CHECK_EQ(platform.GameBase(), std::uintptr_t{0});
}

}  // namespace

// The stand-in for liblog, below, records what the sentinel log writes to logcat.
extern "C" {
int __android_log_write(int prio, const char*, const char* text) {
  LogcatWrites().push_back({prio, text});
  return 1;
}
int __android_log_print(int prio, const char*, const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  LogcatWrites().push_back({prio, buf});
  return 1;
}
}  // extern "C"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: plugin_loader_test <fixture-dir> [name-filter]\n");
    return 2;
  }
  g_dir = argv[1];
  return mini_test::RunAll(argc - 1, argv + 1);
}
