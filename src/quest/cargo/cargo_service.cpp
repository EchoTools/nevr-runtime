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
  // Ok needs a shadow handed to the publisher, from the boot mount of what the store held or from the fetch: a store that is whole but
  // mounts nothing (every served label off, every entry refused) leaves the engine with no asset source, which is the failure this worker is for.
  std::size_t published = 0;
  // Whether the set handed on last left nothing out: no name whose object the store lacks, none refused. The boot mount of the saved
  // manifest is the set that matters when the manifest cannot be refreshed, and then no later set replaces it.
  bool lastSetComplete = false;
  const nevr_cargo::Publish give = [publish, &published, &lastSetComplete](std::unique_ptr<nevr_cargo::Mounted> set) {
    ++published;
    lastSetComplete = set->missing == 0 && set->refused == 0;
    publish(set.release());  // the publisher keeps the set for the life of the process: the engine may be filling a request from its shadow
  };
  env.MakeDirs(held.dir);  // as Start does: the device's free space is asked of a directory that exists
  nevr_cargo::MountFromCache(env, held, give);
  const nevr_cargo::RunResult r = nevr_cargo::FetchAndMount(env, held, give, keep_going);
  nevr_quest::data::AllAssetsResult out;
  out.ok = r.manifest_loaded && r.failed == 0 && std::string(r.stopped).empty() && published != 0;
  // Offline (or the CDN unreachable): the manifest could not be refreshed, but the boot mount handed the engine a complete set from the
  // saved manifest and the objects the store holds, each hashed when it was downloaded. Nothing is missing, so the launch goes on with
  // that set; newer objects of the CDN show at the next launch that reaches it (#2414). A store with a name left out still ends the launch.
  const bool manifestNotRefreshed = std::string(r.stopped) == nevr_cargo::kStoppedManifestFailed && lastSetComplete;
  if (manifestNotRefreshed) {
    out.ok = true;
    env.Event(nevr_cargo::EventLevel::kWarn, "cargo_first_run",
              {{"result", kFirstRunManifestNotRefreshed},
               {"stopped", r.stopped},
               {"published", published},
               {"consequence", "the engine boots on the set the store held at the last launch; objects the CDN published since show at the next launch that reaches it"}});
  }
  if (!out.ok) {
    out.message = kFirstRunFailedMessage;
    env.Event(nevr_cargo::EventLevel::kError, "cargo_first_run",
              {{"result", "failed"},
               {"manifest_loaded", r.manifest_loaded ? "true" : "false"},
               {"failed", r.failed},
               {"stopped", r.stopped},
               {"published", published},
               {"consequence", "the launch ends with a message: the engine cannot start on stub manifests with no asset source"}});
  }
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
