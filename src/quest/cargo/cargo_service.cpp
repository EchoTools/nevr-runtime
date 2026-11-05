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
  nevr_cargo::MountFromCache(env, config, give);
  return nevr_cargo::FetchAndMount(env, config, give, keep_going);
}

}  // namespace nevr_quest::cargo
