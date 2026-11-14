#include "quest/cargo/map_service.h"

#include <time.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/config_alias.h"
#include "core/resource_set_registry.h"
#include "quest/cargo/posix_map_fs.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/map_sets.h"
#include "quest/sentinel/resource_request_hook.h"

namespace nevr_quest::maps {

namespace {

constexpr std::uint32_t kPollMs = 50;

std::atomic<bool> g_started{false};
// The owners this run's boot granted, written once by Start before the mount thread exists; the observer reads them on that thread.
std::vector<std::string> g_granted_owners;

std::string JoinOwners(const std::vector<std::string>& owners) {
  std::string text;
  for (const std::string& owner : owners) text += (text.empty() ? "" : ",") + owner;
  return text.empty() ? "-" : text;
}

void OnMount(const nevr_resource_sets::MountReport& report) {
  const MountOutcome outcome = ApplyMount(g_granted_owners, report);
  sentinel::LogFields(outcome.withdrawn.empty() ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "map_summary",
                      {{"stage", "mount"},
                       {"mounted", outcome.mounted.size()},
                       {"withdrawn", JoinOwners(outcome.withdrawn).c_str()},
                       {"armed", outcome.armed}});
}

struct State {
  nevr_cargo::Env* env = nullptr;
  PosixMapFs fs;
  std::unique_ptr<nevr_map_channel::Channel> channel;
};

// A seam's answer, or none when the platform gave no such seam.
std::vector<std::string> Ask(const std::function<std::vector<std::string>()>& seam) {
  return seam ? seam() : std::vector<std::string>();
}

}  // namespace

nevr_maps::ChannelConfig MakeChannelConfig(const std::string& cache_dir) {
  nevr_maps::ChannelConfig c;
  c.dir = cache_dir;
  c.platform = nevr_maps::kPlatformQuest;
  return c;
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

bool Start(nevr_cargo::Env* env, const nevr_map_channel::Settings& settings, nevr_maps::Deriver* deriver,
           const nevr_map_channel::Seams& seams) noexcept {
  if (env == nullptr || g_started.exchange(true)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "maps_start",
                        {{"result", "refused"}, {"reason", env == nullptr ? "no_env" : "already_started"}});
    return false;
  }
  // Never freed: the thread below and the mounted sets outlive every caller.
  State* const state = new State();
  state->env = env;
  state->channel = std::make_unique<nevr_map_channel::Channel>(MakeChannelConfig(), settings);
  // The score waiver is installed only when the channel runs: a run that does not run has no map script to answer for.
  const bool runs = state->channel->start() == nevr_map_channel::Start::kRun;
  if (runs) nevr_resource_sets::SetScriptWaiver(&HoldsMapScript);
  QuestMounter mounter(/*signed_only=*/false);
  const nevr_map_channel::BootResult& boot = state->channel->Boot(*env, state->fs, mounter, deriver, NowUtc(), {});
  if (boot.start != nevr_map_channel::Start::kRun) {
    return false;  // the core logged why (not_configured or the refusal); nothing was granted, no thread starts
  }
  for (const std::string& id : boot.report.mounted) g_granted_owners.push_back(std::string(nevr_resource_sets::kMapOwnerPrefix) + id);
  map_sets::SetMountObserver(&OnMount);
  nevr_config_alias::InstallIfStaged();
  sentinel::LogFields(sentinel::LogLevel::kInfo, "maps_start",
                      {{"result", "started"},
                       {"mounted", boot.report.mounted.size()},
                       {"deferred", boot.report.deferred.size()}});
  std::thread([state, deriver, seams]() {
    const auto always = [] { return true; };
    MountWhenManifestReady(&resource_request::GameManifestRecords, &resource_request::GameManifestHolds,
                           &resource_request::PublishMapSets, kManifestWaitMs, always);
    // The groups to fetch: the seam's recorded groups, filtered; with no seam, none (the fetch is then not requested this run).
    state->channel->FetchPass(*state->env, state->fs, deriver, nevr_map_channel::FetchGroups(Ask(seams.recorded_groups)),
                              NowUtc(), always);
  }).detach();
  return true;
}

}  // namespace nevr_quest::maps
