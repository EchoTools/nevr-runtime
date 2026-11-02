// Host test for the sentinel's binding of the Quest plugin loader (src/quest/plugin/plugin_runtime.cpp): the one loader of
// the process, planned from the configuration's specs, run at its two moments, ended when the game goes first
// (docs/adr/0019-quest-plugin-loader.md, sections 2 and 3). The decisions are plugin_loader_test.cpp's; this proves the
// binding: the counts the sequence acts on, once-only planning, the records that reach the sentinel's log, and that the
// end-of-process finish never waits on a plugin.
//
// The library directory is the directory of this executable (dladdr), so the fixture shared objects built beside it are the
// "native library directory". They are x86-64 and the runtime loads AArch64, so a fixture that is read is refused for its
// machine type; no plugin code runs here (plugin_loader_test.cpp runs it).
//
// Run: plugin_runtime_test <fixture-dir> [name-filter]

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/plugin/plugin_platform.h"
#include "quest/plugin/plugin_plan.h"
#include "quest/plugin/plugin_runtime.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/mini_test.h"

using namespace nevr_quest::plugin;

namespace {

std::vector<nlohmann::json>& Lines() {
  static std::vector<nlohmann::json> lines;
  return lines;
}
void (*g_onLine)(const nlohmann::json&) = nullptr;

void Capture(sentinel::LogLevel, const char* line) {
  const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
  if (parsed.is_discarded()) return;
  Lines().push_back(parsed);
  if (g_onLine != nullptr) g_onLine(parsed);
}

struct Scope {
  Scope() : previous(sentinel::SetLogSink(&Capture)) {
    Lines().clear();
    g_onLine = nullptr;
    ResetPluginsForTest();
  }
  ~Scope() {
    g_onLine = nullptr;
    ResetPluginsForTest();
    sentinel::SetLogSink(previous);
  }
  sentinel::LogSink previous;
};

nevr::PluginSpec Spec(const std::string& name, bool early = false, bool enabled = true) {
  nevr::PluginSpec spec;
  spec.name = name;
  spec.file = name + ".dll";  // what the shared parser defaults it to
  spec.early = early;
  spec.enabled = enabled;
  return spec;
}

// The `plugin_load` lines, in the order written.
std::vector<nlohmann::json> Records() {
  std::vector<nlohmann::json> out;
  for (const nlohmann::json& line : Lines()) {
    if (line.value("event", "") == "plugin_load") out.push_back(line);
  }
  return out;
}
const nlohmann::json* RecordFor(const std::vector<nlohmann::json>& records, const std::string& name) {
  for (const nlohmann::json& record : records) {
    if (record.value("name", "") == name) return &record;
  }
  return nullptr;
}
int PlanLines() {
  int count = 0;
  for (const nlohmann::json& line : Lines()) count += line.value("event", "") == "plugin_plan" ? 1 : 0;
  return count;
}

TEST(PlanSplitsTheEntriesByMomentAndSettlesWhatThePlanAloneCan) {
  Scope scope;
  nevr::PluginSpec outside = Spec("far");
  outside.file = "sub/libfar.so";
  const PlanCounts counts = PlanPlugins({Spec("e", true), Spec("n"), Spec("off", false, false), outside}, false);
  CHECK_EQ(counts.early, std::size_t{1});
  CHECK_EQ(counts.normal, std::size_t{1});
  const std::vector<nlohmann::json> records = Records();
  CHECK_EQ(records.size(), std::size_t{2});  // the disabled entry and the refused one; nothing was loaded
  const nlohmann::json* off = RecordFor(records, "off");
  const nlohmann::json* far = RecordFor(records, "far");
  CHECK(off != nullptr && far != nullptr);
  if (off == nullptr || far == nullptr) return;
  CHECK_EQ(off->value("outcome", ""), std::string("disabled"));
  CHECK_EQ(far->value("outcome", ""), std::string("refused"));
  CHECK_EQ(far->value("reason", ""), std::string(reason::kFileOutsideDirectory));
  CHECK_EQ(PlanLines(), 1);
}

// The script redirect logs each load that is not redirected only in a development build (script_redirect.h, SetLogEveryLoad):
// planning is where the sentinel learns which build it is, and it is before the dlopen hook goes in.
TEST(PlanningTellsTheScriptRedirectWhetherThisIsADevelopmentBuild) {
  Scope scope;
  nevr_quest::script_redirect::SetLogEveryLoad(false);
  PlanPlugins({}, true);
  CHECK(nevr_quest::script_redirect::LogsEveryLoad());
  ResetPluginsForTest();
  PlanPlugins({}, false);
  CHECK(!nevr_quest::script_redirect::LogsEveryLoad());
  nevr_quest::script_redirect::SetLogEveryLoad(false);
}

TEST(AnEmptyPlanCountsNothingAndWritesOnlyTheSummary) {
  Scope scope;
  const PlanCounts counts = PlanPlugins({}, false);
  CHECK_EQ(counts.early + counts.normal, std::size_t{0});
  CHECK(Records().empty());
  CHECK_EQ(PlanLines(), 1);
}

TEST(PlanningTwiceChangesNothing) {
  Scope scope;
  const std::vector<nevr::PluginSpec> specs = {Spec("n"), Spec("off", false, false)};
  const PlanCounts first = PlanPlugins(specs, false);
  const std::size_t written = Lines().size();
  const PlanCounts second = PlanPlugins({Spec("other"), Spec("another")}, false);
  CHECK_EQ(second.early, first.early);
  CHECK_EQ(second.normal, first.normal);
  CHECK_EQ(Lines().size(), written);
}

TEST(EachMomentLoadsItsOwnEntriesAndEachEntryGetsOneRecord) {
  Scope scope;
  // fx_ok is the x86-64 fixture beside this executable: read, then refused for its machine type. "gone" has no file.
  const PlanCounts counts = PlanPlugins({Spec("fx_ok", true), Spec("gone")}, false);
  CHECK_EQ(counts.early, std::size_t{1});
  CHECK_EQ(counts.normal, std::size_t{1});
  RunEarlyPlugins();
  std::vector<nlohmann::json> records = Records();
  CHECK_EQ(records.size(), std::size_t{1});
  const nlohmann::json* early = RecordFor(records, "fx_ok");
  CHECK(early != nullptr);
  if (early == nullptr) return;
  CHECK_EQ(early->value("phase", ""), std::string("early"));
  CHECK_EQ(early->value("outcome", ""), std::string("refused"));
  CHECK_EQ(early->value("reason", ""), std::string(reason::kMachineType));
  CHECK_EQ(early->value("sha256", "").size(), std::size_t{64});  // the file beside the executable was read
  CHECK_EQ(early->value("file", ""), std::string("libfx_ok.so"));  // the Quest default for name fx_ok
  RunNormalPlugins();
  records = Records();
  CHECK_EQ(records.size(), std::size_t{2});
  const nlohmann::json* normal = RecordFor(records, "gone");
  CHECK(normal != nullptr);
  if (normal == nullptr) return;
  CHECK_EQ(normal->value("phase", ""), std::string("normal"));
  CHECK_EQ(normal->value("outcome", ""), std::string("failed"));
  CHECK_EQ(normal->value("reason", ""), std::string(reason::kFileUnreadable));
  // Nothing is left for the end of the process, and running a moment again writes nothing.
  RunEarlyPlugins();
  RunNormalPlugins();
  FinishPlugins(reason::kGameExitedFirst);
  CHECK_EQ(Records().size(), std::size_t{2});
}

TEST(FinishEndsWhatIsStillPendingOnceWithTheReason) {
  Scope scope;
  PlanPlugins({Spec("a"), Spec("b", true)}, false);
  FinishPlugins(reason::kPostLoadHookUnavailable);
  const std::vector<nlohmann::json> records = Records();
  CHECK_EQ(records.size(), std::size_t{2});
  for (const nlohmann::json& record : records) {
    CHECK_EQ(record.value("outcome", ""), std::string("not_started"));
    CHECK_EQ(record.value("reason", ""), std::string(reason::kPostLoadHookUnavailable));
  }
  FinishPlugins(reason::kGameExitedFirst);
  CHECK_EQ(Records().size(), std::size_t{2});  // a second finish ends nothing again
}

TEST(NothingPlannedMakesEveryMomentAndTheFinishHarmless) {
  Scope scope;
  RunEarlyPlugins();
  RunNormalPlugins();
  FinishPlugins(reason::kGameExitedFirst);
  CHECK(Lines().empty());
}

// The finish runs on the process-end helper thread. While a pass is inside a plugin it must neither wait for the plugin nor
// end the entry the pass is working on; it says so and returns. The sink below plays the other thread: it calls the finish
// from a new thread while the normal pass holds its entry, and waits for it.
void FinishFromAnotherThreadWhileThePassRuns(const nlohmann::json& line) {
  static bool fired = false;
  if (line.value("event", "") != "plugin_load" || fired) return;
  fired = true;
  std::thread other([] { FinishPlugins(reason::kGameExitedFirst); });
  other.join();
}

TEST(TheFinishNeverWaitsForARunningPass) {
  Scope scope;
  PlanPlugins({Spec("first"), Spec("second")}, false);
  g_onLine = &FinishFromAnotherThreadWhileThePassRuns;
  RunNormalPlugins();
  const std::vector<nlohmann::json> records = Records();
  CHECK_EQ(records.size(), std::size_t{2});
  for (const nlohmann::json& record : records) {
    CHECK_EQ(record.value("outcome", ""), std::string("failed"));  // both went through the pass; neither was cut short
  }
  bool busy = false;
  for (const nlohmann::json& line : Lines()) {
    busy = busy || (line.value("event", "") == "plugin_runtime" && line.value("status", "") == "busy");
  }
  CHECK(busy);
}

// ---- the packaged manifest (libnevr_plugin_manifest.so beside the plugins; docs/adr/0019-quest-plugin-loader.md) -------

// The directory the runtime reads plugins from: the directory this library was mapped from, as PlanPlugins finds it.
std::string LibraryDir() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&PlanPlugins), &info) == 0) return "";
  return DirectoryOf(info.dli_fname);
}

// One packaged entry, as the repack writes it. Empty `text` means no manifest file.
std::string OneManifest(bool early, bool enabled, const std::string& sha = std::string(64, '0')) {
  const nlohmann::json entry = {{"name", "fx_ok"}, {"file", "libfx_ok.so"}, {"sha256", sha}, {"enabled", enabled},
                                {"early", early}};
  const nlohmann::json doc = {{"schema", 1}, {"plugins", nlohmann::json::array({entry})}};
  return doc.dump();
}

// Writes the manifest beside the fixture objects (or removes it for empty text); the destructor removes it again.
struct ManifestFile {
  explicit ManifestFile(const std::string& text) : path(LibraryDir() + "/libnevr_plugin_manifest.so") {
    if (text.empty()) {
      std::remove(path.c_str());
      return;
    }
    if (std::FILE* out = std::fopen(path.c_str(), "wb")) {
      std::fwrite(text.data(), 1, text.size(), out);
      std::fclose(out);
    }
  }
  ~ManifestFile() { std::remove(path.c_str()); }
  std::string path;
};

int ManifestLineCount() {
  int count = 0;
  for (const nlohmann::json& line : Lines()) count += line.value("event", "") == "plugin_manifest" ? 1 : 0;
  return count;
}
const nlohmann::json* ManifestLine() {
  for (const nlohmann::json& line : Lines()) {
    if (line.value("event", "") == "plugin_manifest") return &line;
  }
  return nullptr;
}

TEST(PackagedPluginPlansWithNoConfig) {
  Scope scope;
  ManifestFile manifest(OneManifest(false, true));
  const PlanCounts counts = PlanPlugins({}, false);
  CHECK_EQ(counts.normal, std::size_t{1});
  CHECK_EQ(counts.early, std::size_t{0});
}

TEST(ConfigDisablesAPackagedPlugin) {
  Scope scope;
  ManifestFile manifest(OneManifest(false, true));
  PlanPlugins({Spec("fx_ok", false, false)}, false);
  const std::vector<nlohmann::json> records = Records();
  const nlohmann::json* off = RecordFor(records, "fx_ok");
  CHECK(off != nullptr);
  if (off == nullptr) return;
  CHECK_EQ(off->value("outcome", ""), std::string("disabled"));
  CHECK_EQ(off->value("source", ""), std::string("packaged"));
  CHECK_EQ(off->value("overridden", -1), 1);
}

TEST(ManifestMarksAPackagedPluginEarly) {
  Scope scope;
  ManifestFile manifest(OneManifest(true, true));
  const PlanCounts counts = PlanPlugins({}, false);
  CHECK_EQ(counts.early, std::size_t{1});
  CHECK_EQ(counts.normal, std::size_t{0});
}

TEST(ConfigEarlyOnAPackagedNormalPluginMakesItEarly) {
  Scope scope;
  ManifestFile manifest(OneManifest(false, true));
  const PlanCounts counts = PlanPlugins({Spec("fx_ok", true)}, false);
  CHECK_EQ(counts.early, std::size_t{1});
  CHECK_EQ(counts.normal, std::size_t{0});
}

TEST(ConfigOnlyEntryIsUnchanged) {
  Scope scope;
  ManifestFile manifest(OneManifest(false, true));
  PlanPlugins({Spec("gone")}, false);
  RunNormalPlugins();
  const std::vector<nlohmann::json> records = Records();
  const nlohmann::json* gone = RecordFor(records, "gone");
  CHECK(gone != nullptr);
  if (gone == nullptr) return;
  CHECK_EQ(gone->value("outcome", ""), std::string("failed"));
  CHECK_EQ(gone->value("reason", ""), std::string(reason::kFileUnreadable));
  CHECK_EQ(gone->value("source", ""), std::string("config"));
}

TEST(AbsentManifestLogsOnceAndPlansNothing) {
  Scope scope;
  ManifestFile manifest("");
  const PlanCounts counts = PlanPlugins({}, false);
  CHECK_EQ(counts.early + counts.normal, std::size_t{0});
  CHECK(Records().empty());
  CHECK_EQ(ManifestLineCount(), 1);
  const nlohmann::json* line = ManifestLine();
  CHECK(line != nullptr);
  if (line == nullptr) return;
  CHECK_EQ(line->value("status", ""), std::string("absent"));
  CHECK_EQ(line->value("packaged", -1), 0);
}

TEST(InvalidManifestPlansNoPackagedPluginsAndLogsOneError) {
  Scope scope;
  ManifestFile manifest(OneManifest(false, true, "XYZ"));
  PlanPlugins({Spec("gone")}, false);
  RunNormalPlugins();
  CHECK_EQ(ManifestLineCount(), 1);
  const nlohmann::json* line = ManifestLine();
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK_EQ(line->value("status", ""), std::string("invalid"));
    CHECK_EQ(line->value("reason", ""), std::string("bad_sha256"));
    CHECK_EQ(line->value("level", ""), std::string("error"));
    // The line says what follows from it (docs/standards/logging.md, Category G).
    const std::string msg = line->value("msg", "");
    CHECK(msg.find("no packaged plugin") != std::string::npos);
    CHECK(msg.find("config.yaml") != std::string::npos);
  }
  const std::vector<nlohmann::json> records = Records();
  CHECK(RecordFor(records, "fx_ok") == nullptr);  // the packaged entry is not planned
  CHECK(RecordFor(records, "gone") != nullptr);   // the config entry still is
}

TEST(ManifestHashMismatchIsRefusedBeforeTheMachineCheck) {
  Scope scope;
  ManifestFile manifest(OneManifest(false, true, std::string(64, '1')));
  PlanPlugins({}, false);
  RunNormalPlugins();
  const std::vector<nlohmann::json> records = Records();
  const nlohmann::json* fx = RecordFor(records, "fx_ok");
  CHECK(fx != nullptr);
  if (fx == nullptr) return;
  CHECK_EQ(fx->value("outcome", ""), std::string("refused"));
  CHECK_EQ(fx->value("reason", ""), std::string("manifest_hash_mismatch"));
  CHECK_EQ(fx->value("expected_sha256", ""), std::string(64, '1'));  // the manifest's hash, beside the file's own
  CHECK_EQ(fx->value("sha256", "").size(), std::size_t{64});
  CHECK(fx->value("sha256", "") != std::string(64, '1'));
}

// A manifest the loader could not read is not "no packaged plugin": it is a build that has one it cannot see. The status says
// so, at Warning, with the reason and what follows from it.
TEST(UnreadableManifestIsNotReportedAsAbsent) {
  Scope scope;
  const std::string path = LibraryDir() + "/libnevr_plugin_manifest.so";
  CHECK_EQ(::mkdir(path.c_str(), 0700), 0);  // a directory where the file should be
  const PlanCounts counts = PlanPlugins({}, false);
  ::rmdir(path.c_str());
  CHECK_EQ(counts.early + counts.normal, std::size_t{0});
  CHECK_EQ(ManifestLineCount(), 1);
  const nlohmann::json* line = ManifestLine();
  CHECK(line != nullptr);
  if (line == nullptr) return;
  CHECK_EQ(line->value("status", ""), std::string("unreadable"));
  CHECK_EQ(line->value("reason", ""), std::string("not_regular_file"));
  CHECK_EQ(line->value("level", ""), std::string("warn"));
  CHECK_EQ(line->value("packaged", -1), 0);
  const std::string msg = line->value("msg", "");
  CHECK(msg.find("no packaged plugin") != std::string::npos);
  CHECK(msg.find("config.yaml") != std::string::npos);
}

TEST(ASymlinkWhereTheManifestShouldBeIsUnreadableToo) {
  Scope scope;
  const std::string path = LibraryDir() + "/libnevr_plugin_manifest.so";
  CHECK_EQ(::symlink("libfx_ok.so", path.c_str()), 0);
  PlanPlugins({}, false);
  ::unlink(path.c_str());
  const nlohmann::json* line = ManifestLine();
  CHECK(line != nullptr);
  if (line == nullptr) return;
  CHECK_EQ(line->value("status", ""), std::string("unreadable"));
  CHECK_EQ(line->value("reason", ""), std::string("symlink"));
}

TEST(ManifestIsReadOnceAndPlanningTwiceChangesNothing) {
  Scope scope;
  ManifestFile manifest(OneManifest(false, true));
  PlanPlugins({}, false);
  PlanPlugins({Spec("other")}, false);
  CHECK_EQ(ManifestLineCount(), 1);
}

}  // namespace

extern "C" {
// liblog is not linked on the host; what the sentinel's log writes to logcat goes nowhere here.
int __android_log_write(int, const char*, const char*) { return 1; }
int __android_log_print(int, const char*, const char*, ...) { return 1; }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: plugin_runtime_test <fixture-dir> [name-filter]\n");
    return 2;
  }
  return mini_test::RunAll(argc - 1, argv + 1);
}
