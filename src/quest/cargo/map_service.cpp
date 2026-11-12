#include "quest/cargo/map_service.h"

#include <time.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/config_alias.h"
#include "core/minisign_verify.h"
#include "quest/cargo/posix_map_fs.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/map_sets.h"
#include "quest/sentinel/resource_request_hook.h"

namespace nevr_quest::maps {

namespace {

constexpr std::uint32_t kPollMs = 50;

std::atomic<bool> g_started{false};
// The staged pins of this run, written once by Start before the mount thread exists; the observer reads them on that thread.
std::vector<pin::Pin> g_staged_pins;
std::vector<std::string> g_granted_owners;

std::string JoinOwners(const std::vector<std::string>& owners) {
  std::string text;
  for (const std::string& owner : owners) text += (text.empty() ? "" : ",") + owner;
  return text.empty() ? "-" : text;
}

void OnMount(const nevr_resource_sets::MountReport& report) {
  const MountOutcome outcome = ApplyMount(g_granted_owners, report);
  pin::SetMounted(MountedPins(g_staged_pins, report));
  sentinel::LogFields(outcome.withdrawn.empty() ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "map_summary",
                      {{"stage", "mount"},
                       {"mounted", outcome.mounted.size()},
                       {"withdrawn", JoinOwners(outcome.withdrawn).c_str()},
                       {"armed", outcome.armed}});
}

struct State {
  nevr_cargo::Env* env;
  PosixMapFs fs;
  nevr_maps::ChannelConfig config;
  std::vector<std::string> deferred;
};

}  // namespace

nevr_maps::ChannelConfig MakeChannelConfig(const MapsChoice& choice, const std::string& cache_dir) {
  nevr_maps::ChannelConfig c;
  c.dir = cache_dir;
  c.base_url = choice.base_url;
  c.platform = nevr_maps::kPlatformQuest;
  c.game_build = choice.game_build;
  c.subscribed = choice.subscribed;
  c.stock_manifest_sha256 = choice.stock_manifest_sha256;
  c.keys = choice.keys;
  c.is_server = false;
  c.signed_only = choice.signed_only;
  if (choice.cap_bytes != 0) c.cap_bytes = choice.cap_bytes;
  c.channel = choice.channel;
  return c;
}

std::vector<pin::Pin> MountedPins(const std::vector<pin::Pin>& staged, const nevr_resource_sets::MountReport& report) {
  std::vector<pin::Pin> mounted;
  if (report.sets == 0) return mounted;
  for (const pin::Pin& p : staged) {
    const std::string owner = "map:" + p.id;
    bool refused = false;
    for (const auto& r : report.refusals) refused = refused || r.plugin == owner;
    if (!refused) mounted.push_back(p);
  }
  return mounted;
}

bool ChoiceFromConfig(const MapsConfig& config, MapsChoice* out, std::string* bad_key) {
  MapsChoice choice;
  choice.base_url = config.baseUrl;
  choice.channel = config.channel;
  choice.subscribed = config.subscribed;
  choice.cap_bytes = config.capMib << 20;
  const auto add_keys = [&](const std::vector<std::string>& lines, const char* name, bool official) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
      nevr_maps::PinnedKey key;
      key.official = official;
      if (!nevr_minisign::ParsePublicKeyText(lines[i], &key.key)) {
        *bad_key = std::string(name) + "[" + std::to_string(i) + "]";
        return false;
      }
      choice.keys.push_back(key);
    }
    return true;
  };
  if (!add_keys(config.officialKeys, "maps.official_keys", true) ||
      !add_keys(config.trustedKeys, "maps.trusted_keys", false)) {
    return false;
  }
  if (!config.officialKeys.empty() && !config.trustedKeys.empty()) {
    *bad_key = "maps.trusted_keys (mixed_keys: never trusted beside maps.official_keys)";
    return false;
  }
  *out = std::move(choice);
  return true;
}

std::string NowUtc() {
  const time_t now = ::time(nullptr);
  struct tm utc {};
  ::gmtime_r(&now, &utc);
  char text[24];
  ::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return text;
}

bool MountWhenManifestReady(std::uint64_t (*records)(), bool (*has)(std::uint64_t type, std::uint64_t name),
                            void (*publish)(const nevr_shadow::Shadow*), std::uint32_t wait_ms,
                            const std::function<bool()>& keep_going) {
  std::uint32_t waited = 0;
  while (records() == 0) {
    if (waited >= wait_ms || !keep_going()) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "maps_mount",
                          {{"result", "abandoned"},
                           {"waited_ms", waited},
                           {"consequence", "no downloaded map is served this run"}});
      map_sets::Abandon();
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    waited += kPollMs;
  }
  struct HasContext {
    bool (*has)(std::uint64_t type, std::uint64_t name);
  };
  HasContext context{has};
  nevr_resource_sets::StockView stock;
  stock.has = [](void* ctx, std::uint64_t type, std::uint64_t name) { return static_cast<HasContext*>(ctx)->has(type, name); };
  stock.ctx = &context;
  stock.records = records();
  return map_sets::Mount(stock, publish).sets != 0;
}

bool Start(nevr_cargo::Env* env, const MapsChoice& choice, nevr_maps::Deriver* deriver) noexcept {
  if (choice.subscribed.empty() || choice.base_url.empty()) {
    sentinel::LogFields(sentinel::LogLevel::kDebug, "maps_start", {{"result", "off"}, {"reason", "not_configured"}});
    return false;
  }
  if (env == nullptr || g_started.exchange(true)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "maps_start",
                        {{"result", "refused"}, {"reason", env == nullptr ? "no_env" : "already_started"}});
    return false;
  }
  // Never freed: the thread below and the mounted sets outlive every caller.
  State* const state = new State{env, {}, MakeChannelConfig(choice), {}};
  if (!env->MakeDirs(state->config.dir)) {
    delete state;
    sentinel::LogFields(sentinel::LogLevel::kWarn, "maps_start",
                        {{"result", "off"},
                         {"reason", "no_cache_dir"},
                         {"consequence", "no downloaded map is served this run"}});
    return false;
  }
  // Before any grant, as PC5b sets its policy: a map set's script record passes while route B holds that script.
  nevr_resource_sets::SetScriptWaiver(&HoldsMapScript);
  QuestMounter mounter;
  nevr_maps::BootReport boot = nevr_maps::BootMount(*env, state->fs, state->config, mounter, deriver, NowUtc());
  state->deferred = boot.deferred;
  for (const std::string& id : boot.mounted) g_granted_owners.push_back(nevr_resource_sets::kMapOwnerPrefix + id);
  pin::View view = pin::ViewFromBoot(boot, state->config.subscribed);
  g_staged_pins = view.staged;
  pin::SetView(std::move(view));
  map_sets::SetMountObserver(&OnMount);
  nevr_config_alias::InstallIfStaged();
  sentinel::LogFields(sentinel::LogLevel::kInfo, "maps_start",
                      {{"result", "started"}, {"mounted", boot.mounted.size()}, {"deferred", boot.deferred.size()}});
  std::thread([state, deriver]() {
    const auto always = [] { return true; };
    MountWhenManifestReady(&resource_request::GameManifestRecords, &resource_request::GameManifestHolds,
                           &resource_request::PublishMapSets, kManifestWaitMs, always);
    nevr_maps::FetchAll(*state->env, state->fs, state->config, state->deferred, deriver, NowUtc(), always);
  }).detach();
  return true;
}

}  // namespace nevr_quest::maps
