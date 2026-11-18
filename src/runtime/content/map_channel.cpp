// The custom-map channel's PC start (map_channel.h): the Win32 seams and the hooks the core's decisions allow. Every decision is the
// core's (core/map_channel.h). Written against the Win32 API and not built or run under the Quest-only verification order.
#include <winsock2.h>
#include <windows.h>

#include "runtime/content/map_channel.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cdn/cdn_env.h"
#include "core/config_alias.h"
#include "core/logging.h"
#include "core/map_channel.h"
#include "core/map_fetch_queue.h"
#include "core/map_manifest.h"
#include "core/map_request.h"
#include "core/map_request_serve.h"
#include "core/map_session_pin.h"
#include "core/nevr_root.h"
#include "core/resource_set_registry.h"
#include "runtime/cargo/stock_source.h"
#include "cdn/env_windows.h"
#include "cdn/windows_map_fs.h"
#include "runtime/content/script_provider.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/lifecycle/game_image_guard.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/resource_sets.h"
#include "runtime/server/map_hold.h"

namespace nevr_map_channel::pc {
namespace {

constexpr const char* kEnvTag = "[NEVR.MAPS]";  // the tag the PC env writes its records under
constexpr const char* kRekeyCopyDir = "rekeyed";  // below a version folder: the copies of the player's stock scripts served as rekeys
constexpr std::size_t kLockNameHexDigits = 16;  // the root lock's name: the first digits of sha256(root)
constexpr std::uint32_t kStopWaitMs = 2000;     // the fetch's stop waits this long, then detaches (the fetch ends at its next transfer)
constexpr std::uint32_t kServerQueueWaitMs = 500;  // a dedicated server's worker checks for a stop this often while no group is requested

std::string Flat(const char* key) {
  const char* value = NevrCfgGetFlat(key);
  return value != nullptr ? value : "";
}

std::vector<std::string> FlatList(const char* key) {
  const char* value = NevrCfgGetFlatCsv(key);
  return SplitCsv(value != nullptr ? value : "");
}

std::string Utf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string Slashes(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  return path;
}

std::wstring ExeDir() {
  wchar_t exe[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
  if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
  const std::wstring path(exe, n);
  return path.substr(0, path.find_last_of(L"\\/"));
}

std::wstring ParentDir(const std::wstring& dir) {
  const std::size_t cut = dir.find_last_of(L"\\/");
  return cut == std::wstring::npos ? std::wstring() : dir.substr(0, cut);
}

std::string NowUtc() {
  SYSTEMTIME t{};
  GetSystemTime(&t);
  char text[24];
  std::snprintf(text, sizeof text, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return text;
}

std::string Join(const std::vector<std::string>& items) {
  std::string text;
  for (const std::string& item : items) text += (text.empty() ? "" : ",") + item;
  return text.empty() ? "-" : text;
}

// The PC's mounter: the script provider, the resource-set registry and the config aliases, granted in the core's order and withdrawn
// whole (ADR 0048 decisions 5, 6 and 7). The official fact the core hands each script goes to the provider unchanged.
class PcMounter : public nevr_maps::Mounter {
 public:
  bool any_script() const { return any_script_; }

  nevr_maps::Grant Script(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string& sha256,
                          bool official) override {
    // The boot stamped the file against the stamp the fetch recorded after hashing it, so the provider does not rehash (ADR 0045 7).
    return Took(nevr_map_scripts::Register(owner, hash, held, sha256, /*rehash=*/false, /*pinned_bytes=*/0, official));
  }

  nevr_maps::Grant Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                         const std::string& stock_sha256, const std::string& version_dir) override {
    // The copy of the player's stock file is written in a folder of its own below the version's folder (ADR 0048 decision 6), never
    // among the members the fetch stamped (their paths are scripts/<hash16>.dll in the version folder itself).
    std::wstring copy_dir = Wide(version_dir + "/" + kRekeyCopyDir);
    for (wchar_t& c : copy_dir) {
      if (c == L'/') c = L'\\';
    }
    return Took(nevr_map_scripts::RegisterRekey(owner, hash, stock_source, stock_sha256, /*official=*/false, copy_dir));
  }

  nevr_maps::Grant DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held, const std::string& sha256,
                                 const std::string& stock_sha256, bool official) override {
    return Took(nevr_map_scripts::RegisterDerived(owner, hash, held, sha256, stock_sha256, official));
  }

  nevr_maps::Grant Set(const std::string& owner, std::int64_t held, const std::string& label, const std::string& sha256,
                       bool verified_stamp) override {
    nevr_resource_sets::MapFileRequest request;
    request.owner = owner;
    request.label = label;
    request.sha256 = sha256;
    request.rehash = !verified_stamp;  // only a worker passes true (core/resource_set_registry.h)
    const nevr_resource_sets::Outcome outcome = nevr_resource_sets::RegisterFileResourceSet(request, held);
    nevr_maps::Grant grant;
    grant.ok = outcome.ok();
    if (!grant.ok) grant.reason = nevr_resource_sets::RefusalName(outcome.refusal);
    return grant;
  }

  nevr_maps::Grant StageAliases(const std::string& owner, const std::string& id, std::uint64_t main_level,
                                const std::vector<nevr_maps::ConfigAlias>& aliases) override {
    nevr_maps::Grant grant;
    if (aliases.empty()) {
      grant.ok = true;
      return grant;
    }
    std::vector<nevr_config_alias::Pair> pairs;
    pairs.reserve(aliases.size());
    for (const nevr_maps::ConfigAlias& a : aliases) pairs.push_back({a.key, a.alias});
    grant.ok = nevr_config_alias::Stage(owner, id, main_level, pairs) == pairs.size();
    if (!grant.ok) grant.reason = "alias_partial";
    return grant;
  }

  void Withdraw(const std::string& owner) override {
    nevr_map_scripts::Withdraw(owner);
    nevr_config_alias::Unstage(owner);
    nevr_resource_sets::WithdrawFileResourceSet(owner);
  }

 private:
  nevr_maps::Grant Took(const nevr_map_scripts::Outcome& outcome) {
    nevr_maps::Grant grant;
    grant.ok = outcome.ok();
    if (grant.ok) {
      any_script_ = true;
    } else {
      grant.reason = outcome.reason;
    }
    return grant;
  }

  bool any_script_ = false;
};

// The run's channel, heap-owned and never freed: the worker and the mount observer use it for the process's life. The mounter is
// kept here because the channel's mounts (the boot's and a server's) register through it for as long as the process runs.
struct State {
  std::unique_ptr<nevr_cdn::Env> env;
  std::unique_ptr<nevr_maps::MapFs> fs;
  std::unique_ptr<nevr_maps::StockSource> stock;
  std::unique_ptr<nevr_maps::ChannelDeriver> deriver;
  std::unique_ptr<Channel> channel;
  std::unique_ptr<FetchWorker> worker;
  std::unique_ptr<PcMounter> mounter;
  std::unique_ptr<nevr_maps::GroupFetchQueue> queue;  // a dedicated server's requests: the groups its sessions name
  std::unique_ptr<nevr_maps::MapRequests> requests;   // a client's request entry (ADR 0048 decision 16): the session asks for its group and map here
  std::atomic<FetchWorker*> worker_ptr{nullptr};      // the worker a request wakes; set once it starts, cleared when the stop begins
  bool server = false;                                // a dedicated server: its fetches are the groups its sessions name
  std::vector<std::string> deferred;                  // the maps the boot mount deferred: the worker hashes them
  std::mutex granted_mutex;                           // guards granted: the mount observer reads it on the thread that mounts
  std::vector<std::string> granted;                   // the owners this run's mounts granted ("map:<id>")
};
State* g_state = nullptr;

// The mounted owners and their levels, from the registry's report: the entries whose plugin is "map:<id>".
nevr_config_alias::OwnerLevels MountedOwners(const std::string& report_json) {
  nevr_config_alias::OwnerLevels owners;
  const nlohmann::json doc = nlohmann::json::parse(report_json, nullptr, false);
  if (!doc.is_array()) return owners;
  for (const nlohmann::json& set : doc) {
    if (!set.is_object()) continue;
    const auto plugin = set.find("plugin");
    if (plugin == set.end() || !plugin->is_string()) continue;
    const std::string owner = plugin->get<std::string>();
    if (owner.rfind(nevr_resource_sets::kMapOwnerPrefix, 0) != 0) continue;
    std::vector<std::uint64_t> levels;
    const auto listed = set.find("levels");
    if (listed != set.end() && listed->is_array()) {
      for (const nlohmann::json& level : *listed) {
        if (level.is_string()) levels.push_back(std::strtoull(level.get<std::string>().c_str(), nullptr, 16));
      }
    }
    owners.emplace_back(owner, std::move(levels));
  }
  return owners;
}

// The mount observer: the core's decision, with the PC's withdraw and alias record type. Runs on the mounting thread.
void OnMount(const nevr_resource_sets::MountReport&) {
  State* st = g_state;
  if (st == nullptr) return;
  std::vector<std::string> granted;
  {
    std::lock_guard<std::mutex> lock(st->granted_mutex);
    granted = st->granted;
  }
  const MountDecision decision = DecideMount(
      granted, MountedOwners(nevr_resource_sets::ReportJson()),
      [](const std::string& owner) {
        nevr_map_scripts::Withdraw(owner);
        nevr_config_alias::Unstage(owner);
      },
      [](const std::string&, std::uint64_t alias) {
        return nevr_resource_sets::HoldsKey(nevr_config_alias::kAliasRecordTypeWin10, alias);
      });
  Log(decision.withdrawn.empty() ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.MAPS] map_summary stage=mount mounted=%zu withdrawn=%s armed=%zu", decision.mounted.size(),
      Join(decision.withdrawn).c_str(), decision.armed);
}

// The root lock: the name's existence is the lock (one process per cache root). The handle is never closed, so the name lives as long
// as the process does.
bool TryLockRoot(nevr_cdn::Env& env, const std::string& root) {
  std::unique_ptr<nevr_cdn::Hasher> hasher = env.NewHasher();
  hasher->Update(root.data(), root.size());
  const std::wstring name = L"Local\\nevr-maps-" + Wide(hasher->Hex().substr(0, kLockNameHexDigits));
  HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
  if (mutex == nullptr) return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return false;
  }
  return true;
}

// The seams' answers: a platform with no seam (none yet on this piece) answers nothing.
std::vector<std::string> Ask(const std::function<std::vector<std::string>()>& seam) {
  return seam ? seam() : std::vector<std::string>();
}

// Records the owners a mount granted, so the mount observer knows which withdrawals it must check (any thread).
void GrantMounted(State* st, const nevr_maps::MountReport& report) {
  std::lock_guard<std::mutex> lock(st->granted_mutex);
  for (const std::string& id : report.mounted) st->granted.push_back(std::string(nevr_resource_sets::kMapOwnerPrefix) + id);
}

// The request entry the join hold calls (runtime/server/map_hold.h): queues one group's fetch on the server's worker. The queue
// refuses a path-unsafe group id; the join hold holds nothing for a refused request.
bool RequestServerGroup(const std::string& group) {
  State* const st = g_state;
  if (st == nullptr || !st->queue) return false;
  std::string reason;
  return st->queue->Request(group, [](const nevr_maps::FetchReport&) {}, &reason);
}

// One group for a dedicated server (ADR 0048 decisions 13 and 17). Its progress goes to the join hold. When the fetch did not fail,
// its maps mount now, in this process, and the hold hears of the mount. A failed fetch mounts nothing: the hold runs to its timeout,
// and the Warning there says the session started without the maps.
nevr_maps::FetchReport FetchServerGroup(State* st, const std::string& group, const std::function<bool()>& keep_going) {
  const std::string now = NowUtc();
  const nevr_maps::FetchProgressFn progress = [](const nevr_maps::FetchProgress& p) { nevr_map_hold::NoteFetchProgress(p); };
  // The maps this run already mounted (an earlier group's) are never evicted by this group's fetch: the same in-use set FetchPass uses.
  nevr_maps::FetchScope scope;
  scope.in_use = st->channel->MountedIds();
  const nevr_maps::FetchReport report = nevr_maps::FetchGroup(*st->env, *st->fs, st->channel->config(), group, st->deferred,
                                                              st->deriver.get(), now, keep_going, progress, scope);
  if (report.outcome == nevr_maps::FetchOutcome::kFailed) return report;
  const nevr_maps::MountReport mounted = st->channel->MountGroups({group}, now);
  GrantMounted(st, mounted);
  if (st->mounter->any_script()) nevr_map_scripts::InstallMapScriptsRewriter();
  nevr_config_alias::InstallIfStaged();
  nevr_map_hold::NoteGroupMounted(group, mounted);
  return report;
}

// A dedicated server's worker: it waits for a group the join hold requested and fetches it, until the worker is stopped.
void ServerPass(State* st, const std::function<bool()>& keep_going) {
  while (keep_going()) {
    if (!st->queue->WaitForRequest(kServerQueueWaitMs)) continue;
    st->queue->RunNext([st, &keep_going](const std::string& group) { return FetchServerGroup(st, group, keep_going); });
  }
}

// Starts a dedicated server's worker and sets the request entry the join hold calls (the entry is set last, once the worker can take it).
void StartServerWorker(State* st) {
  st->queue = std::make_unique<nevr_maps::GroupFetchQueue>();
  st->worker = std::make_unique<FetchWorker>(
      [st](const std::function<bool()>& keep_going) {
        try {
          ServerPass(st, keep_going);
        } catch (const std::exception& e) {
          Log(EchoVR::LogLevel::Warning,
              "[NEVR.MAPS] map_done reason=exception what=\"%s\" (the server's group is not mounted this run)", e.what());
        }
      },
      [st] { return st->env->NowMs(); });
  nevr_map_hold::SetRequestFetch(&RequestServerGroup);
  st->worker->Start();
}

// A client's request entry: a session success queues its group, and the session pin asks for the map it lacks (both through the
// core's seams). The queued requests run on the fetch worker (DrainRequests), so a request made before the worker starts waits there.
void InstallClientRequests(State* st) {
  st->requests = std::make_unique<nevr_maps::MapRequests>(*st->env);
  st->requests->SetWake([st] {
    if (FetchWorker* const worker = st->worker_ptr.load()) worker->Request();  // a refused one still gets a pass when the gap ends
  });
  nevr_maps::InstallRequestSeams(st->requests.get());
}

void BootUnguarded(std::uintptr_t game_base, bool is_server) {
  const std::wstring root = nevr_root::RootW();
  const Settings settings = ReadSettings([](const char* key) { return Flat(key); },
                                         [](const char* key) { return FlatList(key); }, CompiledOfficialKeys());
  if (DecideStart(settings) == Start::kRun) {
    // Every map hook (the script rewriter, the config alias, the registry's observer) is for one build: the runtime's guard.
    if (!nevr_game_image_guard::IsSupportedGameModule(reinterpret_cast<const void*>(game_base))) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] map_start reason=unsupported_game_build (echovr.exe is not the build the map hooks are for; no map is mounted or "
          "fetched)");
      return;
    }
    if (root.empty()) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_start reason=no_root (LOCALAPPDATA unset or too long)");
      return;
    }
  }

  State* const st = new State();
  st->server = is_server;
  st->env = std::make_unique<nevr_cdn::WindowsEnv>(kEnvTag);
  st->fs = nevr_maps::MakeWinMapFs();
  const std::wstring exe_dir = ExeDir();
  st->stock = nevr_maps::MakeWinStockSource(ParentDir(ParentDir(exe_dir)));
  st->deriver = std::make_unique<nevr_maps::ChannelDeriver>(*st->env, *st->fs, *st->stock);

  nevr_maps::ChannelConfig config;
  config.dir = Slashes(Utf8(nevr_root::MapsCacheDir(root)));
  config.signed_only = nevr_plugin_trust::ActiveMode() == nevr_plugin_trust::Mode::SignedOnly;
  st->channel = std::make_unique<Channel>(config, settings);
  g_state = st;

  // The script policy is the channel's fact (ADR 0048 decision 7), set before any grant, and only when the channel runs.
  if (st->channel->start() == Start::kRun) {
    nevr_cdn::DeviceFacts facts;
    st->env->QueryDevice(config.dir, &facts);
    nevr_map_scripts::SetPolicy(nevr_map_scripts::PolicyCaller::kMaps, config.signed_only, nevr_maps::MapDocumentBytes(facts));
  }

  st->mounter = std::make_unique<PcMounter>();
  // The recorded groups are the session pin's requests and the cache index's groups (RecordedGroupSeams). No local copy is listed
  // here: this build has no reader of hand-installed copies, so local_map_ids is unset and the boot's local list is empty.
  const Seams seams = RecordedGroupSeams(*st->env, st->channel->config().dir);
  const BootResult& boot =
      st->channel->Boot(*st->env, *st->fs, *st->mounter, st->deriver.get(), NowUtc(), Ask(seams.local_map_ids));
  if (boot.start != Start::kRun) return;  // the core wrote why (not_configured, or the refusal)
  ArmSessionGroupRequest();  // a session success now queues its group for the fetch worker (ADR 0048 decision 15)
  if (!st->server) {
    InstallClientRequests(st);  // and the request entry replaces that seam for a client (ADR 0048 decision 16)
    // The session pin reads this run's view from the boot: the maps it lists and the ones it could not mount (K1b, as the Quest does).
    nevr_map_channel::PublishBootView(boot.report, &nevr_map_session_pin::SetChannelView);
  }

  st->deferred = boot.report.deferred;
  GrantMounted(st, boot.report);
  if (st->mounter->any_script()) nevr_map_scripts::InstallMapScriptsRewriter();
  nevr_resource_sets::SetMountObserver(&OnMount);
  nevr_config_alias::InstallIfStaged();
}

// The worker's request pass: each queued group is fetched on demand with the scope its requests asked for, listed, and its listing is
// published as the session pin's view (core/map_request_serve.h, the same pass the Quest runs). Stops when keep_going turns false.
void DrainRequests(State* st, const std::function<bool()>& keep_going) {
  const auto fetch = [st, &keep_going](const std::string& group, const nevr_maps::FetchScope& scope) {
    return nevr_maps::FetchGroup(*st->env, *st->fs, st->channel->config(), group, st->deferred, st->deriver.get(), NowUtc(), keep_going,
                                 nullptr, scope);
  };
  const auto list = [st](const std::string& group) { return st->channel->ListGroups({group}, NowUtc()); };
  nevr_map_channel::ServeSessionRequests(*st->requests, fetch, list, &nevr_map_session_pin::SetChannelView, keep_going);
}

void StartFetchUnguarded() {
  State* const st = g_state;
  if (st == nullptr || st->channel->start() != Start::kRun || st->worker) return;
  const std::string dir = st->channel->config().dir;
  if (!TryLockRoot(*st->env, dir)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_start reason=busy (another game process is fetching into %s)", dir.c_str());
    return;
  }
  if (st->server) {
    StartServerWorker(st);
    return;
  }
  st->worker = std::make_unique<FetchWorker>(
      [st](const std::function<bool()>& keep_going) {
        try {
          DrainRequests(st, keep_going);
          const Seams seams = RecordedGroupSeams(*st->env, st->channel->config().dir);
          st->channel->FetchPass(*st->env, *st->fs, st->deriver.get(), FetchGroups(Ask(seams.recorded_groups)), NowUtc(),
                                 keep_going);
        } catch (const std::exception& e) {
          Log(EchoVR::LogLevel::Warning,
              "[NEVR.MAPS] map_done reason=exception what=\"%s\" (the cache keeps what it had; the next pass starts over)", e.what());
        }
      },
      [st] { return st->env->NowMs(); });
  // The wake pointer is set before the thread starts: a request queued from here on is either taken by the first pass's drain, or
  // its wake reaches a started worker. Set after Start, a request in between would wait for a pass that never comes.
  st->worker_ptr.store(st->worker.get());
  st->worker->Start();
}

}  // namespace

void Boot(std::uintptr_t game_base, bool is_server) {
  static bool s_booted = false;  // the boot thread, once
  if (s_booted) return;
  s_booted = true;
  try {
    BootUnguarded(game_base, is_server);
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_start reason=exception what=\"%s\" (nothing more is mounted or fetched this run)",
        e.what());
  }
}

void StartFetch() {
  try {
    StartFetchUnguarded();
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_start reason=exception what=\"%s\" (no fetch this run)", e.what());
  }
}

void StopFetch() {
  State* const st = g_state;
  if (st == nullptr) return;
  if (st->requests) st->requests->Stop();  // later requests are refused and the waiting ones answered cancelled, before the worker stops
  if (!st->worker) return;
  st->worker_ptr.store(nullptr);
  if (!st->worker->Stop(std::chrono::milliseconds(kStopWaitMs))) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_stop reason=join_timeout (the fetch ends at its next transfer bound)");
  }
}

}  // namespace nevr_map_channel::pc
