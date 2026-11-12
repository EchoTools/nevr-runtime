#include "quest/cargo/cargo_service.h"

#include "quest/cargo/posix_env.h"

namespace nevr_quest::cargo {

nevr_cargo::RunConfig MakeRunConfig(const AssetsChoice& assets, const std::string& cache_dir) {
  nevr_cargo::RunConfig c;
  c.dir = cache_dir;
  c.base_url = assets.cdnUrl.empty() ? std::string(kDefaultBaseUrl) : assets.cdnUrl;
  c.manifest_path = assets.manifest.empty() ? std::string(kDefaultManifestPath) : assets.manifest;
  c.platform = nevr_cargo::kAndroidArm64;
  for (const auto& kv : assets.labels) {
    nevr_cargo::LabelState state;
    if (nevr_cargo::ParseLabelState(kv.second, &state)) c.policy.overrides[kv.first] = state;
  }
  c.policy.required_only = assets.requiredOnly;
  c.source = assets.source == Source::kFile ? "config" : "default";
  return c;
}

nevr_cargo::RunResult RunOnce(nevr_cargo::Env& env, const nevr_cargo::RunConfig& config, PublishFn publish,
                              const std::function<bool()>& keep_going) {
  // The publisher keeps the set for the life of the process: the engine may be filling a request from its shadow.
  const nevr_cargo::Publish give = [publish](std::unique_ptr<nevr_cargo::Mounted> set) { publish(set.release()); };
  env.MakeDirs(config.dir);  // as Start does: the device's free space is asked of a directory that exists
  nevr_cargo::MountFromCache(env, config, give);
  return nevr_cargo::FetchAndMount(env, config, give, keep_going);
}

nevr_quest::data::AllAssetsResult FirstRun(nevr_cargo::Env& env, const nevr_cargo::RunConfig& config, PublishFn publish,
                                           const std::function<bool()>& keep_going) {
  nevr_cargo::RunConfig held = config;
  held.first_run = true;
  const nevr_cargo::RunResult r = RunOnce(env, held, publish, keep_going);
  nevr_quest::data::AllAssetsResult out;
  out.ok = r.manifest_loaded && r.failed == 0 && std::string(r.stopped).empty();
  if (!out.ok) out.message = kFirstRunFailedMessage;
  return out;
}

std::function<nevr_quest::data::AllAssetsResult(const std::string& root)> MakeFirstRunWorker(const AssetsChoice& assets,
                                                                                            const std::string& cache_dir,
                                                                                            nevr_quest::data::Transport* transport,
                                                                                            PublishFn publish) {
  return [assets, cache_dir, transport, publish](const std::string&) {
    PosixEnv env(transport);
    const nevr_cargo::RunConfig config = MakeRunConfig(assets, cache_dir);
    return FirstRun(env, config, publish, [] { return true; });
  };
}

}  // namespace nevr_quest::cargo
