#include "cdn/cdn_service.h"

#include <atomic>
#include <cstring>
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
  if (std::strcmp(platform.name, kWindowsX64.name) == 0) {
    for (const char* label : kClientBootOnlyLabels) c.boot_only.push_back(label);
  }
  return c;
}

RunConfig ResolveManifestPath(Env& env, const RunConfig& config, const std::function<bool()>& keep_going) {
  if (config.manifest_path != kDefaultManifestPath) return config;
  long status = 0;
  const HttpResult http = env.Get(config.base_url + config.manifest_path,
                                  [&status](long st, const void*, std::size_t) {
                                    status = st;
                                    return false;  // the status is the answer; the fetch itself reads the body
                                  },
                                  keep_going);
  if ((status != 0 ? status : http.status) != kManifestMissingStatus) return config;
  env.Event(EventLevel::kInfo, "cdn_manifest",
            {{"result", "fallback"},
             {"from", "v2"},
             {"to", "v1"},
             {"status", kManifestMissingStatus},
             {"consequence", "labels with scripts are not offered; v1's labels mount as before"}});
  RunConfig legacy = config;
  legacy.manifest_path = kLegacyManifestPath;
  return legacy;
}

RunResult RunOnce(Env& env, const RunConfig& config, PublishFn publish, const std::function<bool()>& keep_going) {
  // The publisher keeps the set for the life of the process: the engine may be filling a request from its shadow.
  const Publish give = [publish](std::unique_ptr<Mounted> set) { publish(set.release()); };
  env.MakeDirs(config.dir);  // as Start does: the device's free space is asked of a directory that exists
  BootMount mounted;
  MountFromCache(env, config, give, &mounted);
  RunConfig fetch = ResolveManifestPath(env, config, keep_going);
  fetch.boot = std::move(mounted.boot);  // the set the boot mount published: the fetch keeps its boot-only labels
  return FetchAndMount(env, fetch, give, keep_going);
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
  const RunResult r = FetchAndMount(env, ResolveManifestPath(env, held, keep_going), give, keep_going);
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
  // The client's boot half runs on this thread (PLAN-v7 5.4 "Client start"). It asks the CDN which manifest to fetch only when its boot
  // fetch will run, under that fetch's own ceiling (ClientBoot), so a launch whose cache holds the boot-only labels makes no request here.
  // Never freed: the detached thread reads it for the life of the process.
  RunConfig* const held = new RunConfig(config);
  BootMount boot = ClientBoot(*env, *held, give);
  held->boot = std::move(boot.boot);  // the set the boot mount published: the fetch keeps its boot-only labels
  const bool resolvedAtBoot = !boot.resolved_manifest_path.empty();
  if (resolvedAtBoot) held->manifest_path = boot.resolved_manifest_path;
  // A boot half that asked the CDN does not ask again; a launch with no boot fetch asks here, as the fetch always did.
  std::thread([env, held, give, resolvedAtBoot]() {
    const auto keepGoing = [] { return true; };
    FetchAndMount(*env, resolvedAtBoot ? *held : ResolveManifestPath(*env, *held, keepGoing), give, keepGoing);
  }).detach();
  return true;
}

}  // namespace nevr_cdn
