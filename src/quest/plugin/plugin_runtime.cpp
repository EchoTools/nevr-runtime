#include "quest/plugin/plugin_runtime.h"

#include <dlfcn.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "quest/plugin/packaged_plugins.h"
#include "quest/plugin/plugin_loader.h"
#include "quest/plugin/plugin_platform.h"
#include "quest/plugin/script_redirect.h"
#include "quest/sentinel/hook_log.h"
#include "runtime/ext/plugin_load_plan_build.h"

namespace nevr_quest::plugin {

namespace {

// The loader and its platform live in one heap object that is never destroyed: a plugin's code stays mapped for the life of
// the process, and a namespace-scope object with a destructor is what tools/check_quest_static_init.sh rejects.
struct Runtime {
  // One pass at a time: the early pass, the normal pass and the end-of-process finish run on three different threads. The
  // pass holds it through the plugins' own init, so the finish only takes it when no pass is running.
  std::mutex mutex;
  SentinelPlatform platform;
  Loader loader;
  Runtime(std::string libraryDir, bool devBuild, bool configRejected)
      : platform(std::move(libraryDir), devBuild), loader(platform, LoaderOptions(configRejected)) {}
  static Options LoaderOptions(bool configRejected) {
    Options options;
    options.configRejected = configRejected;
    return options;
  }
};

Runtime*& Instance() {
  static Runtime* instance = nullptr;
  return instance;
}

// The directory this library was mapped from; "" when the loader cannot say (the plan then reads no file and every entry
// goes to dlopen by name with its hash unavailable).
std::string OwnDirectory() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&PlanPlugins), &info) == 0) return "";
  return DirectoryOf(info.dli_fname);
}

// The packaged entries: the manifest the repack wrote beside the plugins (kManifestFile), read once per process. A file that is
// not there is the normal case for a build with no packaged plugin; a file that is there and cannot be used (unreadable, or
// not a manifest this loader understands) means the build has plugins this run will not load, so it is logged at Warning or
// Error with what follows from it. Either way no packaged entry is planned and the config entries still are.
constexpr const char* kNoPackagedMessage =
    "no packaged plugin loads from this APK; entries in config.yaml still load as source=config";

std::vector<PackagedPlugin> ReadPackaged(SentinelPlatform& platform) {
  std::vector<std::uint8_t> bytes;
  const char* readWhy = "";
  const ReadOutcome read = platform.ReadFileOutcome(kManifestFile, &bytes, &readWhy);
  if (read == ReadOutcome::kAbsent) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "plugin_manifest",
                        {{"status", "absent"}, {"packaged", 0}, {"file", kManifestFile}});
    return {};
  }
  if (read == ReadOutcome::kUnreadable) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "plugin_manifest",
                        {{"status", "unreadable"}, {"packaged", 0}, {"reason", readWhy}, {"file", kManifestFile},
                         {"msg", kNoPackagedMessage}});
    return {};
  }
  const std::string text(bytes.begin(), bytes.end());
  std::vector<PackagedPlugin> packaged;
  std::string why;
  if (!ParseManifest(text, &packaged, &why)) {
    sentinel::LogFields(sentinel::LogLevel::kError, "plugin_manifest",
                        {{"status", "invalid"}, {"packaged", 0}, {"reason", why.c_str()}, {"file", kManifestFile},
                         {"msg", kNoPackagedMessage}});
    return {};
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "plugin_manifest",
                      {{"status", "read"}, {"packaged", packaged.size()}, {"file", kManifestFile}});
  return packaged;
}

PlanCounts CountsOf(const Loader& loader) {
  PlanCounts counts;
  for (const PlanEntry& entry : loader.Entries()) {
    if (!entry.pending) continue;
    if (entry.item.early) {
      ++counts.early;
    } else {
      ++counts.normal;
    }
  }
  return counts;
}

void Contained(const char* what, const std::exception& error) noexcept {
  try {
    sentinel::LogFields(sentinel::LogLevel::kError, "plugin_runtime", {{"step", what}, {"status", "exception"}});
    static_cast<void>(error);
  } catch (const std::exception&) {
    // A log line that cannot be written changes nothing the game does.
  }
}

}  // namespace

PlanCounts PlanPlugins(const std::vector<nevr::PluginSpec>& specs, bool devBuild, bool configRejected) noexcept {
  try {
    if (Instance() == nullptr) {
      Instance() = new Runtime(OwnDirectory(), devBuild, configRejected);
      // The script redirect's per-load lines are a development build's, like a plugin's DEBUG lines (script_redirect.h).
      script_redirect::SetLogEveryLoad(devBuild);
      // Packaged entries first, then config.yaml: config overrides a packaged plugin in place, or names an unpackaged one
      // (MergePackaged, docs/adr/0019-quest-plugin-loader.md section 2).
      const std::vector<PackagedPlugin> packaged = ReadPackaged(Instance()->platform);
      Instance()->loader.Plan(MergePackaged(packaged, nevr_plugincfg::BuildLoadPlanFromSpecs(specs)));
    }
    const std::lock_guard<std::mutex> lock(Instance()->mutex);
    return CountsOf(Instance()->loader);
  } catch (const std::exception& error) {
    Contained("plan", error);
    return {};
  }
}

void RunEarlyPlugins() noexcept {
  try {
    if (Instance() == nullptr) return;
    const std::lock_guard<std::mutex> lock(Instance()->mutex);
    Instance()->loader.RunPhase(PluginPhase::Early);
  } catch (const std::exception& error) {
    Contained("early", error);
  }
}

void RunNormalPlugins() noexcept {
  try {
    if (Instance() == nullptr) return;
    const std::lock_guard<std::mutex> lock(Instance()->mutex);
    Instance()->loader.RunPhase(PluginPhase::Normal);
  } catch (const std::exception& error) {
    Contained("normal", error);
  }
}

void FinishPlugins(const char* reasonToken) noexcept {
  try {
    if (Instance() == nullptr) return;
    // A pass is running (a plugin's init is on another thread): it settles its own entry, and the process is ending, so the
    // finish does not wait for a plugin. Entries still pending then have no record; the log says why.
    std::unique_lock<std::mutex> lock(Instance()->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "plugin_runtime", {{"step", "finish"}, {"status", "busy"}});
      return;
    }
    Instance()->loader.Finish(reasonToken);
  } catch (const std::exception& error) {
    Contained("finish", error);
  }
}

void RefreshMainLevel() noexcept {
  try {
    if (Instance() == nullptr) return;
    // Called from the game's update hook: it never waits. A pass holding the loader (a plugin's init on another thread)
    // skips this frame; the next frame refreshes.
    std::unique_lock<std::mutex> lock(Instance()->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    Instance()->loader.RefreshMainLevel();
  } catch (const std::exception& error) {
    Contained("refresh", error);
  }
}

void ResetPluginsForTest() noexcept {
  delete Instance();
  Instance() = nullptr;
}

}  // namespace nevr_quest::plugin
