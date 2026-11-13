/* The CDN texture delivery on the Quest (docs/adr/0045-cdn-texture-delivery.md D3 and D4): at start it mounts what the cache holds, with
 * no network, and publishes it to the resolver hook; then one background thread fetches the manifest, the blobs it names, writes the
 * index and publishes what arrived. The game's frame never waits for it. A first launch may show the stock texture; a second launch
 * shows the CDN texture from its first load.
 *
 * The code holds every default (the host, the manifest path, the label states); `assets.*` in config.yaml only changes them
 * (quest_config.h AssetsChoice). The cache is app-private: `<internal files>/nevr/cache`.
 *
 * Built without exceptions: the fetch runs beside the game, and the core it drives is built the same way.
 */
#pragma once

#include <functional>
#include <string>

#include "core/cargo_run.h"
#include "quest/data/data_step.h"
#include "quest/sentinel/quest_config.h"

namespace nevr_quest::cargo {

inline constexpr const char* kDefaultCacheDir = "/data/data/com.readyatdawn.r15/files/nevr/cache";
inline constexpr const char* kDefaultBaseUrl = "https://cargo.echo.taxi/";
inline constexpr const char* kDefaultManifestPath = "v1/manifest.json.zst";  // one zstd frame: nothing uncompressed is published

// The run's settings: the code's defaults, changed by what the config file gave.
nevr_cargo::RunConfig MakeRunConfig(const AssetsChoice& assets, const std::string& cache_dir = kDefaultCacheDir);

// Receives each mounted set. The set is never freed (a request being filled from its shadow must stay valid).
using PublishFn = void (*)(const nevr_cargo::Mounted*);

// The boot half and then the fetch, on the calling thread: the whole of what the background thread does. For the host tests.
nevr_cargo::RunResult RunOnce(nevr_cargo::Env& env, const nevr_cargo::RunConfig& config, PublishFn publish,
                              const std::function<bool()>& keep_going);

// Shown when the whole-game launch's download could not finish: the engine cannot start on stub manifests with no asset source, so the
// launch ends with this message (data_step's `worker_refused`). The next launch resumes from the objects already stored.
inline constexpr const char* kFirstRunFailedMessage =
    "Echo VR could not download its game data. Check the connection and free space on the headset, then start the game again.";

// The whole-game launch's setup (data_wiring::Options::all_assets_ready, docs/adr/0045 decision 11), on the calling thread, before the
// engine starts: mounts what the cache holds, then downloads every object of the manifest through the first-run prefetch and mounts the
// result. Ok only when the store is whole and a manifest was loaded; otherwise the message the player reads. For the host tests.
nevr_quest::data::AllAssetsResult FirstRun(nevr_cargo::Env& env, const nevr_cargo::RunConfig& config, PublishFn publish,
                                           const std::function<bool()>& keep_going);

// The function the sentinel hands the data step, over a transport and a cache directory: it makes the POSIX env, publishes to
// `publish` (the resource request hook in production), and runs FirstRun with the configuration's settings. The transport must outlive every call.
std::function<nevr_quest::data::AllAssetsResult(const std::string& root)> MakeFirstRunWorker(const AssetsChoice& assets,
                                                                                            const std::string& cache_dir,
                                                                                            nevr_quest::data::Transport* transport,
                                                                                            PublishFn publish);

// Production: MakeFirstRunWorker over the libcurl transport (never freed) and kDefaultCacheDir.
std::function<nevr_quest::data::AllAssetsResult(const std::string& root)> MakeProductionFirstRunWorker(const AssetsChoice& assets);

// Production: mounts what the cache holds, publishes it to the resource request hook, and starts one detached thread for the fetch.
// False (with one logged line) when it was started before; true otherwise, also when the cache directory cannot be made (the delivery
// is then off for this run and says so).
bool Start(const AssetsChoice& assets) noexcept;

}  // namespace nevr_quest::cargo
