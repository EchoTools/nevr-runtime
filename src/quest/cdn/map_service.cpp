#include "quest/cdn/map_service.h"

#include <time.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/config_alias.h"
#include "core/map_channel.h"
#include "core/map_request.h"
#include "core/map_request_serve.h"
#include "core/map_session_pin.h"
#include "core/resource_set_registry.h"
#include "cdn/posix_map_fs.h"
#include "quest/cdn/map_pin_game.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/map_sets.h"
#include "quest/sentinel/process_end_hook.h"
#include "quest/sentinel/resource_request_hook.h"

// The process end's map stop runs inside the hook's before-exit budget; a wait as long as the budget would let the hook's _exit cut the
// report that follows it and the stop's own map_stop record.
static_assert(nevr_quest::maps::kProcessEndStopWait.count() < nevr_quest::process_end::kFlushBudgetMs,
              "the process end's map stop must leave the before-exit budget room for the report");

namespace nevr_quest::maps {

namespace {

constexpr std::uint32_t kPollMs = 50;

std::atomic<bool> g_started{false};
// The owners this run's boot granted, written once by Start before the mount thread exists; the observer reads them on that thread.
std::vector<std::string>& g_granted_owners() {
  static std::vector<std::string> instance;  // function-local: no load-time initializer in the sentinel (tools/check_quest_static_init.sh)
  return instance;
}

std::string JoinOwners(const std::vector<std::string>& owners) {
  std::string text;
  for (const std::string& owner : owners) text += (text.empty() ? "" : ",") + owner;
  return text.empty() ? "-" : text;
}

void OnMount(const nevr_resource_sets::MountReport& report) {
  const MountOutcome outcome = ApplyMount(g_granted_owners(), report);
  // The registry's mount completed: a join held for one of these maps replays its level load (a no-op until the Quest installs a hold).
  nevr_map_session_pin::NoteMapsMounted(outcome.mounted);
  sentinel::LogFields(outcome.withdrawn.empty() ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "map_summary",
                      {{"stage", "mount"},
                       {"mounted", outcome.mounted.size()},
                       {"withdrawn", JoinOwners(outcome.withdrawn).c_str()},
                       {"armed", outcome.armed}});
}

struct State {
  nevr_cdn::Env* env = nullptr;
  PosixMapFs fs;
  std::unique_ptr<nevr_map_channel::Channel> channel;
  std::unique_ptr<QuestMounter> mounter;  // the boot's cache mount keeps a reference to it, so it lives as long as the state
  std::unique_ptr<nevr_maps::MapRequests> requests;  // the sessions' requests (core/map_request.h); the seams call into it for the process
  std::unique_ptr<nevr_map_channel::FetchWorker> worker;  // one pass, then parked until a request (core/map_channel.h)
  std::atomic<nevr_map_channel::FetchWorker*> worker_ptr{nullptr};  // set before the worker starts, so a request from then on wakes it
  bool manifest_waited = false;  // the first pass waits for the game's manifest once; only the worker's thread reads it
};

// The running channel's state for Stop: published by Start once its worker runs; null when the channel never ran.
std::atomic<State*> g_state{nullptr};

// A seam's answer, or none when the platform gave no such seam.
std::vector<std::string> Ask(const std::function<std::vector<std::string>()>& seam) {
  return seam ? seam() : std::vector<std::string>();
}

}  // namespace

nevr_maps::ChannelConfig MakeChannelConfig(const std::string& cache_dir) {
  nevr_maps::ChannelConfig c;
  c.dir = cache_dir;
  c.platform = nevr_maps::kPlatformQuest;
  c.mount_on_demand = nevr_maps::MountOnDemandFor(/*is_server=*/false);  // the Quest has no dedicated server
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

namespace {

// The game's stock lookup as a StockView; `context` must outlive the call it is used in.
struct HasContext {
  bool (*has)(std::uint64_t type, std::uint64_t name);
};
nevr_resource_sets::StockView StockOf(HasContext* context, std::uint64_t records) {
  nevr_resource_sets::StockView stock;
  stock.has = [](void* ctx, std::uint64_t type, std::uint64_t name) { return static_cast<HasContext*>(ctx)->has(type, name); };
  stock.ctx = context;
  stock.records = records;
  return stock;
}

}  // namespace

bool MountWhenManifestReady(std::uint64_t (*records)(), bool (*has)(std::uint64_t type, std::uint64_t name),
                            void (*publish)(const nevr_shadow::Shadow*), std::uint32_t wait_ms,
                            const std::function<bool()>& keep_going) {
  std::uint32_t waited = 0;
  // KNOWN-WRONG #2764: a manifest that is not ready within wait_ms abandons the run's map registration for good. Every map now mounts
  // through it (ADR 0048 decision 16, on demand), so each later request's set is refused too_late, the pin tells the player "did not
  // load" with that token, and nothing retries: the wait runs once per process (State::manifest_waited). Only a restart recovers.
  while (records() == 0) {
    if (waited >= wait_ms || !keep_going()) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "maps_mount",
                          {{"result", "abandoned"},
                           {"waited_ms", waited},
                           {"consequence",
                            "no downloaded map is served this run: later requests are refused too_late and are not retried, restart the "
                            "game to mount a map"}});
      map_sets::Abandon();
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    waited += kPollMs;
  }
  HasContext context{has};
  return map_sets::Mount(StockOf(&context, records()), publish).sets != 0;
}

bool MountLaterNow(std::uint64_t (*records)(), bool (*has)(std::uint64_t type, std::uint64_t name),
                   void (*publish)(const nevr_shadow::Shadow*)) {
  const std::uint64_t count = records();
  if (count == 0) return false;
  HasContext context{has};
  return map_sets::MountLater(StockOf(&context, count), publish).sets != 0;
}

// The worker's mount of the groups a pass or a request stored (ADR 0048 decision 16), on this thread: the cache mount grants each map's
// set, the registry mounts it beside the earlier ones and the resolver is given the one new shadow. `asked` limits the mount under
// mount_on_demand (ChannelConfig), as on the PC.
nevr_maps::MountReport MountGranted(State* state, const std::vector<std::string>& groups, const std::vector<std::string>& asked) {
  const nevr_maps::MountReport mounted = state->channel->MountGroups(groups, NowUtc(), asked);
  g_granted_owners().clear();  // the observer decides the grants of this mount only
  for (const std::string& id : mounted.mounted) {
    g_granted_owners().push_back(std::string(nevr_resource_sets::kMapOwnerPrefix) + id);
  }
  nevr_config_alias::InstallIfStaged();
  MountLaterNow(&resource_request::GameManifestRecords, &resource_request::GameManifestHolds, &resource_request::PublishMapSets);
  return mounted;
}

// The edge's mount of a group's asked ids and its listing for the pin: the served requests' mount (ServeSessionRequests) and the light
// job's (ServeMountOnly) are the same.
nevr_map_channel::GroupMount GroupMountOf(State* state) {
  return [state](const std::string& group, const std::vector<std::string>& asked) { return MountGranted(state, {group}, asked); };
}
nevr_map_channel::GroupListing GroupListingOf(State* state) {
  return [state](const std::string& group) { return state->channel->ListGroups({group}, NowUtc()); };
}

bool Start(nevr_cdn::Env* env, const nevr_map_channel::Settings& settings, nevr_maps::Deriver* deriver) noexcept {
  if (env == nullptr || g_started.exchange(true)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "maps_start",
                        {{"result", "refused"}, {"reason", env == nullptr ? "no_env" : "already_started"}});
    return false;
  }
  // Never freed: the worker below and the mounted sets outlive every caller.
  State* const state = new State();
  state->env = env;
  state->channel = std::make_unique<nevr_map_channel::Channel>(MakeChannelConfig(), settings);
  // The score waiver is installed only when the channel runs: a run that does not run has no map script to answer for.
  const bool runs = state->channel->start() == nevr_map_channel::Start::kRun;
  if (runs) nevr_resource_sets::SetScriptWaiver(&HoldsMapScript);
  state->mounter = std::make_unique<QuestMounter>(/*signed_only=*/false);
  const nevr_map_channel::BootResult& boot = state->channel->Boot(*env, state->fs, *state->mounter, deriver, NowUtc(), {});
  if (boot.start != nevr_map_channel::Start::kRun) {
    return false;  // the core logged why (not_configured or the refusal); nothing was granted, no thread starts
  }
  for (const std::string& id : boot.report.mounted) g_granted_owners().push_back(std::string(nevr_resource_sets::kMapOwnerPrefix) + id);
  // A session success asks for its group, and the session pin for a map of it the Quest lacks: both land in the request entry, which
  // the worker serves below (core/map_request.h, the PC's InstallClientRequests). The pin reads the boot's view until a pass lists more.
  state->requests = std::make_unique<nevr_maps::MapRequests>(*env);
  nevr_maps::InstallRequestSeams(state->requests.get());
  nevr_map_channel::PublishBootView(boot.report, &nevr_map_session_pin::SetChannelView);
  pin::InstallTick();
  map_sets::SetMountObserver(&OnMount);
  nevr_config_alias::InstallIfStaged();
  sentinel::LogFields(sentinel::LogLevel::kInfo, "maps_start",
                      {{"result", "started"},
                       {"mounted", boot.report.mounted.size()},
                       {"deferred", boot.report.deferred.size()}});
  // The worker's pass: the manifest wait (once), the session requests, then the background pass over the groups the cache holds.
  // A request's fetch is on demand (only the map it asked for, and the held maps are updated), and its group is listed after it, so the
  // pin's view names what arrived and what plays after the next start (the map sets mount once per run: quest/sentinel/map_sets.h).
  // Like the PC's, the worker keeps the core's gap between extra passes (kExtraFetchGapMs).
  nevr_map_channel::FetchWorker::Pass pass = [state, deriver](const std::function<bool()>& keep_going) {
    if (!state->manifest_waited) {
      state->manifest_waited = true;
      MountWhenManifestReady(&resource_request::GameManifestRecords, &resource_request::GameManifestHolds,
                             &resource_request::PublishMapSets, kManifestWaitMs, keep_going);
    }
    const nevr_map_channel::GroupFetch fetch = [state, deriver, &keep_going](const std::string& group,
                                                                             const nevr_maps::FetchScope& scope) {
      return nevr_maps::FetchGroup(*state->env, state->fs, state->channel->config(), group, state->channel->TakeDeferred(), deriver, NowUtc(),
                                   keep_going, nullptr, scope);
    };
    // A map a request fetched mounts in this run (the core decides it, ServeSessionRequests), through the same MountGranted as the
    // post-pass block below.
    nevr_map_channel::ServeSessionRequests(*state->requests, fetch, GroupListingOf(state), &nevr_map_session_pin::SetChannelView, keep_going,
                                           GroupMountOf(state));
    const nevr_map_channel::Seams seams = nevr_map_channel::RecordedGroupSeams(*state->env, state->channel->config().dir);
    const std::vector<std::string> groups = nevr_map_channel::FetchGroups(Ask(seams.recorded_groups));
    const std::vector<nevr_maps::FetchReport> reports = state->channel->FetchPass(*state->env, state->fs, deriver, groups, NowUtc(), keep_going);
    // What the pass stored mounts in this run (ADR 0048 decision 16), on this thread: the cache mount grants each map's set, the
    // registry mounts it beside the earlier ones and the resolver is given the one new shadow.
    const std::vector<std::string> stored = nevr_map_channel::StoredGroupsToMount(reports, state->channel->config().mount_on_demand);
    if (stored.empty()) return;
    MountGranted(state, stored, {});
  };
  state->worker = std::make_unique<nevr_map_channel::FetchWorker>(std::move(pass), [state] { return state->env->NowMs(); });
  // A request for a map stored at the listed version is a mount with no fetch: the light job runs it at once, whatever the gap says.
  state->worker->SetLight([state] {
    nevr_map_channel::ServeMountOnly(*state->requests, GroupMountOf(state), GroupListingOf(state), &nevr_map_session_pin::SetChannelView);
  });
  state->requests->SetWakeLight([state] {
    if (nevr_map_channel::FetchWorker* const worker = state->worker_ptr.load()) worker->RequestLight();
  });
  // The wake is set before the worker starts: a request queued from here on is taken by the first pass, or its wake reaches a started worker.
  state->requests->SetWake([state] {
    if (nevr_map_channel::FetchWorker* const worker = state->worker_ptr.load()) worker->Request();  // a refused one still gets a pass when the gap ends
  });
  state->worker_ptr.store(state->worker.get());
  state->worker->Start();
  g_state.store(state);
  return true;
}

void Stop(std::chrono::milliseconds wait) noexcept {
  State* const state = g_state.load();
  if (state == nullptr) return;
  // The core's stop, shared with the PC's StopFetch (core/map_channel.h StopChannel): the requests first, then the worker.
  if (!nevr_map_channel::StopChannel(state->requests.get(), state->worker.get(), &state->worker_ptr, wait)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "map_stop",
                        {{"reason", "join_timeout"}, {"consequence", "the fetch ends at its next transfer bound"}});
  }
}

}  // namespace nevr_quest::maps
