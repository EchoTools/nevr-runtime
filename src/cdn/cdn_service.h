/* The CDN service (docs/adr/0045-cdn-texture-delivery.md D3 and D4), one implementation for every platform: the run's settings from
 * the code's defaults and what the config gave, the boot mount and the fetch on one thread (RunOnce), the whole-game first run
 * (FirstRun), and the production start (Start): mount what the cache holds with no network, publish it, then one background thread
 * fetches the manifest and the objects it names and publishes what arrived. The game's frame never waits for it.
 *
 * Everything the machine does goes through nevr_cdn::Env (cdn_env.h): env_posix.h on the Quest, env_windows.h on the PC, a fake in the
 * tests. A runtime's start (the Quest's quest/cdn/cdn_start.cpp, the PC's runtime/lifecycle/cdn_start.cpp) makes its Env, its
 * publisher and its cache directory, and calls Start.
 *
 * Built without exceptions: the fetch runs beside the game, and the core it drives is built the same way.
 */
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cdn/cdn_env.h"
#include "cdn/cdn_manifest.h"
#include "cdn/cdn_run.h"

namespace nevr_cdn {

inline constexpr const char* kDefaultBaseUrl = "https://cargo.echo.taxi/";
inline constexpr const char* kDefaultManifestPath = "v1/manifest.json.zst";  // one zstd frame: nothing uncompressed is published

// What the config file said about the CDN (`assets.*`); an empty field means the code's default. Each runtime fills it from its own
// config reader.
struct Settings {
  std::string cdn_url;                                      // the base URL, ending in '/'
  std::string manifest;                                     // relative to the base URL
  bool required_only = false;
  std::vector<std::pair<std::string, std::string>> labels;  // (label, "on" | "off" | "required"), in file order
  bool from_config = false;                                 // the config file gave at least one accepted key
};

// The run's settings: the code's defaults, changed by what the config gave, for `platform`'s entries of the manifest.
RunConfig MakeRunConfig(const Settings& settings, const std::string& cache_dir, const Platform& platform);

// Receives each mounted set. The set is never freed (a request being filled from its shadow must stay valid).
using PublishFn = void (*)(const Mounted*);

// The boot half and then the fetch, on the calling thread: the whole of what the background thread does. For the tests.
RunResult RunOnce(Env& env, const RunConfig& config, PublishFn publish, const std::function<bool()>& keep_going);

// Shown when the whole-game launch's download could not finish: the engine cannot start on stub manifests with no asset source, so the
// launch ends with this message. The next launch resumes from the objects already stored.
inline constexpr const char* kFirstRunFailedMessage =
    "Echo VR could not download its game data. Check the connection and free space on the headset, then start the game again.";

// The `result` of the Warning `cdn_first_run` when the manifest could not be refreshed and the launch went on with the complete set the store held (#2414).
inline constexpr const char* kFirstRunManifestNotRefreshed = "manifest_not_refreshed";

struct FirstRunResult {
  bool ok = false;
  std::string message;  // what the player reads when !ok
};

// The whole-game launch's setup (docs/adr/0045 decision 11), on the calling thread, before the engine starts: mounts what the cache
// holds, then downloads every object of the manifest through the first-run prefetch and mounts the result. Ok when the store is whole
// and a manifest was loaded, or when the manifest request failed and the set mounted from the saved manifest left no name out (a
// Warning says the manifest was not refreshed, #2414); otherwise kFirstRunFailedMessage.
FirstRunResult FirstRun(Env& env, const RunConfig& config, PublishFn publish, const std::function<bool()>& keep_going);

// Production: makes the cache directory, mounts what it holds, publishes it, and starts one detached thread for the fetch. `env` is
// never freed (the thread and the published sets outlive every caller). False (with one `cdn_start` Warning) when a start ran before
// in this process; true otherwise, also when the cache directory cannot be made (the delivery is then off for this run, and a
// `cdn_start` Warning says so).
bool Start(Env* env, const RunConfig& config, PublishFn publish) noexcept;

}  // namespace nevr_cdn
