#include "cdn/cdn_service.h"

#include <atomic>
#include <memory>
#include <thread>

namespace nevr_cdn {

namespace {

std::atomic<bool> g_started{false};

}  // namespace

RunConfig MakeRunConfig(const Settings& settings, const std::string& cache_dir, const Platform& platform) {
  RunConfig c;
  c.dir = cache_dir;
  c.base_url = settings.cdn_url.empty() ? std::string(kDefaultBaseUrl) : settings.cdn_url;
  c.manifest_path = settings.manifest.empty() ? std::string(kDefaultManifestPath) : settings.manifest;
  c.platform = platform;
  for (const auto& kv : settings.labels) {
    LabelState state;
    if (ParseLabelState(kv.second, &state)) c.policy.overrides[kv.first] = state;
  }
  c.policy.required_only = settings.required_only;
  c.source = settings.from_config ? "config" : "default";
  return c;
}

RunResult RunOnce(Env& env, const RunConfig& config, PublishFn publish, const std::function<bool()>& keep_going) {
  // The publisher keeps the set for the life of the process: the engine may be filling a request from its shadow.
  const Publish give = [publish](std::unique_ptr<Mounted> set) { publish(set.release()); };
  env.MakeDirs(config.dir);  // as Start does: the device's free space is asked of a directory that exists
  MountFromCache(env, config, give);
  return FetchAndMount(env, config, give, keep_going);
}

FirstRunResult FirstRun(Env& env, const RunConfig& config, PublishFn publish, const std::function<bool()>& keep_going) {
  RunConfig held = config;
  held.first_run = true;
  // Ok needs a shadow handed to the publisher, from the boot mount of what the store held or from the fetch: a store that is whole but
  // mounts nothing (every served label off, every entry refused) leaves the engine with no asset source, which is the failure this worker is for.
  std::size_t published = 0;
  // Whether the set handed on last left nothing out: no name whose object the store lacks, none refused. The boot mount of the saved
  // manifest is the set that matters when the manifest cannot be refreshed, and then no later set replaces it.
  bool lastSetComplete = false;
  const Publish give = [publish, &published, &lastSetComplete](std::unique_ptr<Mounted> set) {
    ++published;
    lastSetComplete = set->missing == 0 && set->refused == 0;
    publish(set.release());  // the publisher keeps the set for the life of the process: the engine may be filling a request from its shadow
  };
  env.MakeDirs(held.dir);  // as Start does: the device's free space is asked of a directory that exists
  MountFromCache(env, held, give);
  const RunResult r = FetchAndMount(env, held, give, keep_going);
  FirstRunResult out;
  out.ok = r.manifest_loaded && r.failed == 0 && std::string(r.stopped).empty() && published != 0;
  // Offline (or the CDN unreachable): the manifest could not be refreshed, but the boot mount handed the engine a complete set from the
  // saved manifest and the objects the store holds, each hashed when it was downloaded. Nothing is missing, so the launch goes on with
  // that set; newer objects of the CDN show at the next launch that reaches it (#2414). A store with a name left out still ends the launch.
  const bool manifestNotRefreshed = std::string(r.stopped) == kStoppedManifestFailed && lastSetComplete;
  if (manifestNotRefreshed) {
    out.ok = true;
    env.Event(EventLevel::kWarn, "cdn_first_run",
              {{"result", kFirstRunManifestNotRefreshed},
               {"stopped", r.stopped},
               {"published", published},
               {"consequence", "the engine boots on the set the store held at the last launch; objects the CDN published since show at the next launch that reaches it"}});
  }
  if (!out.ok) {
    out.message = kFirstRunFailedMessage;
    env.Event(EventLevel::kError, "cdn_first_run",
              {{"result", "failed"},
               {"manifest_loaded", r.manifest_loaded ? "true" : "false"},
               {"failed", r.failed},
               {"stopped", r.stopped},
               {"published", published},
               {"consequence", "the launch ends with a message: the engine cannot start on stub manifests with no asset source"}});
  }
  return out;
}

bool Start(Env* env, const RunConfig& config, PublishFn publish) noexcept {
  if (g_started.exchange(true)) {
    env->Event(EventLevel::kWarn, "cdn_start", {{"result", "refused"}, {"reason", "already_started"}});
    return false;
  }
  if (!env->MakeDirs(config.dir)) {
    env->Event(EventLevel::kWarn, "cdn_start",
               {{"result", "off"},
                {"reason", "no_cache_dir"},
                {"consequence", "no CDN asset is served this run; the game's own data shows"}});
    return true;
  }
  const Publish give = [publish](std::unique_ptr<Mounted> set) { publish(set.release()); };
  MountFromCache(*env, config, give);
  // Never freed: the detached thread reads it for the life of the process.
  const RunConfig* const held = new RunConfig(config);
  std::thread([env, held, give]() { FetchAndMount(*env, *held, give, [] { return true; }); }).detach();
  return true;
}

}  // namespace nevr_cdn
