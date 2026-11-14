#include "quest/cdn/cdn_worker.h"

#include "cdn/env_posix.h"

namespace nevr_quest::cdn {

nevr_cdn::Settings SettingsFrom(const AssetsChoice& assets) {
  nevr_cdn::Settings s;
  s.cdn_url = assets.cdnUrl;
  s.manifest = assets.manifest;
  s.required_only = assets.requiredOnly;
  s.labels = assets.labels;
  s.from_config = assets.source == Source::kFile;
  return s;
}

nevr_cdn::RunConfig MakeRunConfig(const AssetsChoice& assets, const std::string& cache_dir) {
  return nevr_cdn::MakeRunConfig(SettingsFrom(assets), cache_dir, nevr_cdn::kAndroidArm64);
}

std::function<nevr_quest::data::AllAssetsResult(const std::string& root)> MakeFirstRunWorker(const AssetsChoice& assets,
                                                                                            const std::string& cache_dir,
                                                                                            nevr_quest::data::Transport* transport,
                                                                                            nevr_cdn::PublishFn publish) {
  return [assets, cache_dir, transport, publish](const std::string&) {
    nevr_cdn::PosixEnv env(transport);
    const nevr_cdn::FirstRunResult r = nevr_cdn::FirstRun(env, MakeRunConfig(assets, cache_dir), publish, [] { return true; });
    nevr_quest::data::AllAssetsResult out;
    out.ok = r.ok;
    out.message = r.message;
    return out;
  };
}

}  // namespace nevr_quest::cdn
