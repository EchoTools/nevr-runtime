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
    void* handle = ::dlopen((g_dir + "/" + file).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle != nullptr) handles[file] = handle;
    return handle;
  }
  void* Symbol(void* handle, const char* name) override { return ::dlsym(handle, name); }
  std::string GameBuildId() override { return gameBuild; }
  std::uintptr_t GameBase() override { return base; }
  void Emit(const LoadRecord& record) override { records.push_back(record); }
  void EmitPlan(std::size_t entries, std::size_t early, std::size_t normal, std::size_t refused,
                std::size_t disabled) override {
    plans.push_back({entries, early, normal, refused, disabled});
  }
  void PluginLine(std::uint32_t level, const std::string& line) override { lines.push_back({level, line}); }

  FxSeen* Seen(const std::string& file) const {
    const auto it = handles.find(file);
    return it == handles.end() ? nullptr : static_cast<FxSeen*>(::dlsym(it->second, "fx_seen"));
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
  std::uintptr_t base = 0x7000000000;
  bool readFromDisk = true;
  std::string throwOnRead;
  std::map<std::string, std::vector<std::uint8_t>> memory;
  std::map<std::string, void*> handles;
  std::vector<std::string> opened;
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
  const std::vector<std::uint8_t> bytes = Slurp(g_dir + "/libfx_ok.so");
  CHECK(!bytes.empty());
  CHECK_EQ(r->sha256, nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()));
  CHECK_EQ(r->sha256.size(), std::size_t{64});
  CHECK_EQ(r->pluginBuildId, BuildIdOf(bytes.data(), bytes.size()));
  CHECK_EQ(r->pluginBuildId.size(), std::size_t{40});  // the fixtures are linked with --build-id=sha1
}

TEST(LoaderGivesThePluginTheQuestContext) {
  Env env;
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
  CHECK_EQ(std::string(seen->game_build_id), std::string(kGameBuild));
  // No source for the main level exists on Quest before the host tick: the fields are empty, not wrong.
  CHECK_EQ(seen->level_symbol, std::uint64_t{0});
  CHECK(seen->level_name == nullptr);
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
  const FxSeen* seen = env.platform.Seen("libfx_otherids.so");
  CHECK(seen != nullptr);
  if (seen == nullptr) return;
  CHECK_EQ(seen->init_count, 0u);  // refused before init
  CHECK_EQ(seen->ctor_ran, 1u);    // dlopen ran its constructors: the ADR's order, and the static-init check's reason
}

TEST(LoaderRefusesAPluginWithoutTheBuildIdExport) {
  Env env;
  env.loader->Plan({Item("noids", "libfx_noids.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("noids")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("noids")->reason, std::string(reason::kBuildIdsUndeclared));
  CHECK_EQ(env.platform.Seen("libfx_noids.so")->init_count, 0u);
}

TEST(LoaderRefusesEveryPluginWhenTheRunningBuildCannotBeRead) {
  Env env;
  env.platform.gameBuild = "";
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("ok")->outcome == Outcome::kRefused);
  CHECK_EQ(env.platform.Record("ok")->reason, std::string(reason::kGameBuildIdUnavailable));
  CHECK_EQ(env.platform.Record("ok")->gameBuildId, std::string("none"));
  CHECK_EQ(env.platform.Seen("libfx_ok.so")->init_count, 0u);
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

TEST(LoaderStillTriesToMapAFileItCannotRead) {
  Env env;
  env.platform.readFromDisk = false;  // the library directory is not a directory (an APK-mapped path)
  env.loader->Plan({Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  const LoadRecord* r = env.platform.Record("ok");
  CHECK(r->outcome == Outcome::kLoaded);
  CHECK_EQ(r->sha256, std::string("unavailable"));
  CHECK_EQ(r->pluginBuildId, std::string("unavailable"));
}

TEST(LoaderReportsAMissingFileAsADlopenFailure) {
  Env env;
  env.loader->Plan({Item("gone", "libfx_missing.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("gone")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("gone")->reason, std::string(reason::kDlopenFailed));
}

TEST(LoaderFailsAPluginWithoutGetInfo) {
  Env env;
  env.loader->Plan({Item("noinfo", "libfx_noinfo.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("noinfo")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("noinfo")->reason, std::string(reason::kNoPluginInfo));
  CHECK_EQ(env.platform.Seen("libfx_noinfo.so")->init_count, 0u);
}

TEST(LoaderFailsAPluginWhoseInitFailsAndContinues) {
  Env env;
  env.loader->Plan({Item("initfail", "libfx_initfail.so"), Item("ok", "libfx_ok.so")});
  env.loader->RunPhase(PluginPhase::Normal);
  CHECK(env.platform.Record("initfail")->outcome == Outcome::kFailed);
  CHECK_EQ(env.platform.Record("initfail")->reason, std::string(reason::kInitFailed));
  CHECK_EQ(env.platform.Seen("libfx_initfail.so")->shutdown_count, 1u);  // the documented lifecycle
  CHECK(env.platform.Record("ok")->outcome == Outcome::kLoaded);
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
  record.phase = PluginPhase::Early;
  record.outcome = Outcome::kRefused;
  record.reason = reason::kBuildIdUnsupported;
  record.required = true;
  record.argKeys = "region,token";
  LogLoadRecord(record);
  CHECK_EQ(Captured().size(), std::size_t{1});
  if (Captured().empty()) return;
  const nlohmann::json line = nlohmann::json::parse(Captured()[0]);
  CHECK_EQ(line.value("event", ""), std::string("plugin_load"));
  CHECK_EQ(line.value("level", ""), std::string("warn"));
  CHECK_EQ(line.value("name", ""), std::string("ok"));
  CHECK_EQ(line.value("file", ""), std::string("libok.so"));
  CHECK_EQ(line.value("sha256", ""), std::string(64, 'a'));
  CHECK_EQ(line.value("plugin_build_id", ""), std::string("bb"));
  CHECK_EQ(line.value("game_build_id", ""), std::string("cc"));
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
