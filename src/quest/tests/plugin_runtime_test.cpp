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

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/plugin/plugin_plan.h"
#include "quest/plugin/plugin_runtime.h"
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
  CHECK_EQ(normal->value("reason", ""), std::string(reason::kDlopenFailed));
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
