/* The Quest's start of the CDN service (cdn/cdn_service.h, docs/adr/0045-cdn-texture-delivery.md D3 and D4): only what is the Quest's
 * own. The cache directory, the config reader (AssetsChoice), the data step's result type, the POSIX env over the data fetch's
 * transport, and the resource request hook as the publisher. The service itself (the mount, the fetch, the first run) is the core's.
 *
 * The code holds every default (the host, the manifest path, the label states); `assets.*` in config.yaml only changes them
 * (quest_config.h AssetsChoice). The cache is app-private: `<internal files>/nevr/cache`.
 *
 * Built without exceptions: the fetch runs beside the game, and the core it drives is built the same way.
 */
#pragma once

#include <functional>
#include <string>

#include "cdn/cdn_service.h"
#include "quest/data/data_step.h"
#include "quest/sentinel/quest_config.h"

namespace nevr_quest::cdn {

inline constexpr const char* kDefaultCacheDir = "/data/data/com.readyatdawn.r15/files/nevr/cache";

// The config file's `assets.*` as the core's settings.
nevr_cdn::Settings SettingsFrom(const AssetsChoice& assets);

// The run's settings on the Quest: the core's defaults, changed by what the config file gave, for the Android entries of the manifest.
nevr_cdn::RunConfig MakeRunConfig(const AssetsChoice& assets, const std::string& cache_dir = kDefaultCacheDir);

// The function the sentinel hands the data step (data_wiring::Options::all_assets_ready, docs/adr/0045 decision 11), over a transport
// and a cache directory: it makes the POSIX env, publishes to `publish` (the resource request hook in production), and runs
// nevr_cdn::FirstRun with the configuration's settings; a failure's message is data_step's `worker_refused`. The transport must
// outlive every call.
std::function<nevr_quest::data::AllAssetsResult(const std::string& root)> MakeFirstRunWorker(const AssetsChoice& assets,
                                                                                            const std::string& cache_dir,
                                                                                            nevr_quest::data::Transport* transport,
                                                                                            nevr_cdn::PublishFn publish);

// Production: MakeFirstRunWorker over the libcurl transport (never freed) and kDefaultCacheDir.
std::function<nevr_quest::data::AllAssetsResult(const std::string& root)> MakeProductionFirstRunWorker(const AssetsChoice& assets);

// Production: nevr_cdn::Start over the POSIX env and the libcurl transport (both never freed), kDefaultCacheDir and the resource
// request hook's publish.
bool Start(const AssetsChoice& assets) noexcept;

}  // namespace nevr_quest::cdn
