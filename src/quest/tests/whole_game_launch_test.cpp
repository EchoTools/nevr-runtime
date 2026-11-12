// Host test for #2398: a whole-game launch (the data step in its all-assets mode, `assets.whole_game`) reaches the first-run download and
// then the mount before the step returns, instead of logging `data_all_assets_ready result=skipped reason=no_worker`.
//
// The step is run as the sentinel runs it: StepConfig::all_assets on, and all_assets_ready the function the sentinel sets
// (nevr_quest::cargo::MakeFirstRunWorker, the one MakeProductionFirstRunWorker wraps over libcurl), here over a scripted transport and
// a scratch cache. The sentinel's own line that sets it (src/quest/sentinel/entry.cpp) is checked by tools/tests/test_whole_game_wiring.py.
//
// Built and run by `just test-quest-data`.

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/cargo/cargo_service.h"
#include "quest/data/data_step.h"
#include "quest/data/sha256.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/cargo_zstd_support.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;
namespace qc = nevr_quest::cargo;
namespace qd = nevr_quest::data;
using json = nlohmann::ordered_json;
using nevr_quest::data::Sha256Hex;

constexpr const char* kBase = "https://cargo.echo.taxi/";
constexpr const char* kManifestPath = "v1/allassets/test/manifest.json.zst";
constexpr std::size_t kDescriptorBytes = 248;
constexpr std::size_t kPixelBytes = 8388608;
constexpr const char* kType = "0x489bb35d53ca50e9";  // the platform's payload type (nc::kAndroidArm64)

std::mutex g_mutex;
std::vector<std::string> g_cargoLines;  // the sentinel's JSON lines (the cargo worker and the Quest env log through them)
void Sink(sentinel::LogLevel, const char* line) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_cargoLines.emplace_back(line);
}
// The first line of `event` (that also has `field` equal to `value`, when one is named); lines.size() when there is none.
std::size_t FirstIndexOf(const std::vector<std::string>& lines, const std::string& event, const char* field = nullptr, const char* value = nullptr) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const json j = json::parse(lines[i], nullptr, false);
    if (j.is_discarded() || j.value("event", "") != event) continue;
    if (field == nullptr || j.value(field, "") == value) return i;
  }
  return lines.size();
}
void PrintLog(const char* title, const std::vector<std::string>& step) {
  if (std::getenv("NEVR_CARGO_PRINT_LOG") == nullptr) return;  // the sentinel's own lines of a launch, for a commit message or a person
  std::printf("== %s\n", title);
  for (const std::string& line : step) std::printf("%s\n", line.c_str());
  for (const std::string& line : g_cargoLines) std::printf("%s\n", line.c_str());
}

struct Scripted : qd::Transport {
  std::map<std::string, std::string> server;
  std::vector<std::string> requests;
  qd::TransportResult GetRange(const std::string& url, std::uint64_t, const qd::BodyFn& on_body, const qd::KeepGoingFn& keep_going) override {
    requests.push_back(url);
    qd::TransportResult r;
    const auto it = server.find(url);
    const long status = it == server.end() ? 404 : 206;
    const std::string body = it == server.end() ? std::string() : it->second;
    std::size_t at = 0;
    r.http_status = status;
    while (at < body.size()) {
      if (!keep_going()) {
        r.interrupted = true;
        return r;
      }
      const std::size_t n = std::min<std::size_t>(4096, body.size() - at);
      if (!on_body(status, body.data() + at, n)) return r;
      at += n;
    }
    if (body.empty() && !on_body(status, "", 0)) return r;
    r.completed = true;
    return r;
  }
};

bool RealExists(const char* path) noexcept {
  struct stat st;
  return ::stat(path, &st) == 0;
}

struct Launch {
  data_test::Scratch scratch;
  Scripted transport;
  std::string descriptor, pixels;
  qd::StepConfig config;
  std::vector<std::string> stepEvents;  // the data step's events, in order, as "name key=value ..."
  std::vector<std::string> published;   // what the worker handed to the resource request hook

  Launch() {
    descriptor = cargo_test::MakeFrame(kDescriptorBytes, 40);
    pixels = cargo_test::MakeFrame(kPixelBytes, 2000);
    const auto entry = [](const char* type, const std::string& blob, std::uint64_t bytes) {
      return json{{"platform", "android-arm64"}, {"type", type}, {"name", "0x3fa6d25bc24a7b01"}, {"format", "zstd-frame"},
                  {"sha256", Sha256Hex(blob.data(), blob.size())}, {"size", blob.size()}, {"bytes", bytes}, {"align", 16}, {"stock_size", bytes}};
    };
    const json doc = {{"schema", "nevr-cargo-manifest"},
                      {"schema_version", 1},
                      {"labels", {{"game", {{"entries", {entry("0xe2efe7289d5985b8", descriptor, kDescriptorBytes), entry(kType, pixels, kPixelBytes)}}}}}}};
    transport.server[std::string(kBase) + kManifestPath] = cargo_test::ZstdFrameOf(doc.dump());
    transport.server[std::string(kBase) + "blobs/sha256/" + Sha256Hex(descriptor.data(), descriptor.size())] = descriptor;
    transport.server[std::string(kBase) + "blobs/sha256/" + Sha256Hex(pixels.data(), pixels.size())] = pixels;

    config.exists = &RealExists;
    config.build_id = "whole-game-launch-test";
    config.fetch_root_override = scratch.Path("root");
    config.recovery_roots = {scratch.Path("root")};  // never the real /sdcard roots
    config.all_assets = true;
    config.on_step = [this](const qd::Event& e) { stepEvents.push_back(Line(e)); };
    config.on_step_warn = [this](const qd::Event& e) { stepEvents.push_back(Line(e)); };
  }
  static std::string Line(const qd::Event& e) {
    std::string line = e.name;
    for (const auto& f : e.fields) line += " " + f.first + "=" + f.second;
    return line;
  }
  bool StepSaid(const std::string& needle) const {
    for (const std::string& l : stepEvents) {
      if (l.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

std::vector<const nc::Mounted*> g_published;
void Publish(const nc::Mounted* m) { g_published.push_back(m); }

// The worker the sentinel sets, over the scripted transport and the scratch cache.
std::function<qd::AllAssetsResult(const std::string&)> TheSentinelsWorker(Launch& launch) {
  nevr_quest::AssetsChoice assets;
  assets.manifest = kManifestPath;
  assets.wholeGame = true;
  return qc::MakeFirstRunWorker(assets, launch.scratch.Path("nevr/cache"), &launch.transport, &Publish);
}

void AWholeGameLaunchDownloadsEveryObjectAndMountsTheShadowBeforeTheStepReturns() {
  g_cargoLines.clear();
  g_published.clear();
  Launch launch;
  launch.config.all_assets_ready = TheSentinelsWorker(launch);
  const qd::StepResult r = qd::RunDataStep(launch.config, launch.transport);
  std::vector<std::string> step;
  for (const std::string& l : launch.stepEvents) step.push_back("step " + l);
  PrintLog("a whole-game launch, every object stored", step);

  QCHECK(r.outcome == qd::StepOutcome::kStubbed);
  // The step called the worker and it answered ok: not the no_worker Warning.
  QCHECK(launch.StepSaid("data_all_assets_ready result=ok reason=worker_done"));
  QCHECK(!launch.StepSaid("no_worker") && !launch.StepSaid("data_all_assets_ready result=skipped"));
  // The download: the manifest, then each object once, all inside the step.
  QCHECK(launch.transport.requests.size() == 3);
  const std::size_t start = FirstIndexOf(g_cargoLines, "cargo_prefetch_start");
  const std::size_t done = FirstIndexOf(g_cargoLines, "cargo_prefetch_done");
  const std::size_t mounted = FirstIndexOf(g_cargoLines, "cargo_mounted", "source", "fetch");  // the boot mount of an empty store says none
  QCHECK(start < done && done < mounted && mounted < g_cargoLines.size());  // the download ends before the mount, and both ran
  if (!(mounted < g_cargoLines.size())) return;
  const json doneLine = json::parse(g_cargoLines[done], nullptr, false);
  QCHECK(doneLine["result"] == "complete" && doneLine["fetched"] == 2);
  const json mountLine = json::parse(g_cargoLines[mounted], nullptr, false);
  QCHECK(mountLine["source"] == "fetch" && mountLine["records"] == 2);
  // The mount was published, and it reads what the download stored.
  QCHECK(g_published.size() == 1 && g_published[0]->shadow != nullptr);
  QCHECK(data_test::ReadFile(launch.scratch.Path("nevr/cache/store-0.pack")) == launch.descriptor + launch.pixels);
}

void ADownloadThatCannotFinishEndsTheLaunchWithTheMessageNotTheNoWorkerWarning() {
  g_cargoLines.clear();
  g_published.clear();
  Launch launch;
  launch.transport.server.erase(std::string(kBase) + "blobs/sha256/" + Sha256Hex(launch.pixels.data(), launch.pixels.size()));  // a 404
  launch.config.all_assets_ready = TheSentinelsWorker(launch);
  const qd::StepResult r = qd::RunDataStep(launch.config, launch.transport);
  QCHECK(r.outcome == qd::StepOutcome::kFailed && r.status == qd::FetchStatus::kWorkerRefused);
  QCHECK(r.message == qc::kFirstRunFailedMessage);
  QCHECK(launch.StepSaid("data_all_assets_ready result=failed reason=worker_refused") && !launch.StepSaid("no_worker"));
  QCHECK(FirstIndexOf(g_cargoLines, "cargo_prefetch_failed") < g_cargoLines.size());
  QCHECK(g_published.empty());  // nothing mounted from a store that is not whole
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  AWholeGameLaunchDownloadsEveryObjectAndMountsTheShadowBeforeTheStepReturns();
  ADownloadThatCannotFinishEndsTheLaunchWithTheMessageNotTheNoWorkerWarning();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "whole_game_launch_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("whole_game_launch_test: ok\n");
  return 0;
}
