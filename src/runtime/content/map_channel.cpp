// The custom-map channel's game half (map_channel.h): config, the image guard, boot order, the observer, the worker.
#include <winsock2.h>
#include <windows.h>

#include "runtime/content/map_channel.h"

#include <algorithm>
#include <cwchar>
#include <exception>

#include "core/config_alias.h"
#include "core/logging.h"
#include "core/map_derive.h"
#include "core/nevr_root.h"
#include "runtime/cargo/win_env.h"
#include "runtime/content/config_alias_hook.h"
#include "runtime/content/script_provider.h"
#include "runtime/content/stock_source_win.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/lifecycle/game_image_guard.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/resource_sets.h"

namespace nevr_map_channel {
namespace {

using namespace detail;
using nevr_map_session_pin::MapPin;

// Heap-owned and never freed: a worker that outlives StopFetch still uses the env, fs, stock source and deriver
// (AUDIT4 D7). The boot's StockCurrent uses the same objects.
struct Channel {
  std::unique_ptr<nevr_cargo::Env> env, local_env;
  std::unique_ptr<nevr_maps::MapFs> fs;
  std::unique_ptr<nevr_maps::StockSource> stock;
  std::unique_ptr<nevr_maps::ChannelDeriver> deriver;
  nevr_maps::ChannelConfig config;
  bool cdn = false, busy = false;
  std::vector<std::string> deferred;
  std::unique_ptr<FetchWorker> worker;
  MountPlan plan;
};
Channel* g_channel = nullptr;

// The compiled-in official content keys: none until the official key is settled (CDN_DESIGN §13 Q4).
const std::vector<nevr_maps::PinnedKey>& OfficialKeys() {
  static const std::vector<nevr_maps::PinnedKey> keys;
  return keys;
}

std::string Flat(const char* key) {
  const char* v = NevrCfgGetFlat(key);
  return v != nullptr ? v : "";
}
std::string FlatCsv(const char* key) {
  const char* v = NevrCfgGetFlatCsv(key);
  return v != nullptr ? v : "";
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

std::string Join(const std::vector<std::string>& items) {
  std::string text;
  for (const std::string& item : items) text += (text.empty() ? "" : ",") + item;
  return text.empty() ? "-" : text;
}

nevr_maps::Grant RegisterSet(const std::string& owner, std::int64_t held, const std::string& label,
                             const std::string& sha256) {
  // The boot stamped the file against the stamp recorded after the fetch hashed it: no rehash on the boot thread.
  const nevr_resource_sets::Outcome o = nevr_resource_sets::RegisterFileResourceSet(owner, held, label, sha256, false);
  return nevr_maps::Grant{o.ok(), o.ok() ? std::string() : std::string(nevr_resource_sets::RefusalName(o.refusal))};
}

std::string ReportJsonText() { return nevr_resource_sets::ReportJson(); }

void OnMount(const nevr_resource_sets::MountReport&) {
  if (g_channel == nullptr) return;
  const MountOutcome o = ApplyMount(g_channel->plan, nevr_resource_sets::ReportJson());
  Log(o.withdrawn.empty() ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.MAPS] map_summary stage=mount mounted=%zu withdrawn=%s armed=%zu", o.mounted.size(),
      Join(o.withdrawn).c_str(), o.armed);
}

std::vector<std::string> AllowIds(const std::vector<std::string>& server_allow) {
  std::vector<std::string> ids;
  for (const std::string& entry : server_allow) ids.push_back(entry.substr(0, entry.find('@')));
  return ids;
}

MapPin PinOf(const nevr_maps::MountedMap& m) {
  return MapPin{m.id, m.version, m.index_sha256, m.platform, m.source_sha256, m.title, m.level};
}

}  // namespace

namespace {

void BootUnguarded(std::uintptr_t game_base, bool is_server) {
  static bool s_booted = false;
  if (s_booted) return;
  s_booted = true;

  const std::wstring root = nevr_root::RootW();
  const Settings settings = ReadSettings(&Flat, &FlatCsv, OfficialKeys());
  if (!settings.reason.empty() && settings.reason != "not_configured") {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_start reason=%s detail=\"%s\" (no map is mounted or fetched)",
        settings.reason.c_str(), settings.detail.c_str());
    return;
  }
  const std::string local_dir = root.empty() ? std::string() : Slashes(Utf8(root)) + "/maps/local";
  std::unique_ptr<nevr_cargo::Env> local_env = nevr_cargo::MakeWinEnv(kLocalTag);
  std::string dev_key;
  const bool local_present = !local_dir.empty() && local_env->ReadSmallFile(local_dir + "/" + kDevKeyFile, &dev_key, 4096);
  if (settings.reason == "not_configured" && !local_present) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_start reason=not_configured");
    return;
  }
  // Every map hook (the script rewriter, the config alias, the session pin) is for one build: the runtime's guard.
  // PC8 and PC9 also check each target's own bytes before they hook it.
  if (!nevr_game_image_guard::IsSupportedGameModule(reinterpret_cast<const void*>(game_base))) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MAPS] map_start reason=unsupported_game_build (echovr.exe is not the build the map hooks are for; no map "
        "is mounted or fetched)");
    return;
  }
  if (root.empty()) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_start reason=no_root (LOCALAPPDATA unset or too long)");
    return;
  }

  Channel* ch = new Channel();
  ch->env = nevr_cargo::MakeWinEnv("[NEVR.MAPS]");
  ch->local_env = std::move(local_env);
  ch->fs = nevr_maps::MakeWinMapFs();
  const std::wstring exe_dir = ExeDir();
  ch->stock = nevr_stock_source::MakeWinStockSource(ParentDir(ParentDir(exe_dir)));
  ch->deriver = std::make_unique<nevr_maps::ChannelDeriver>(*ch->env, *ch->fs, *ch->stock);
  ch->cdn = settings.reason.empty();
  if (ch->cdn) {
    for (const nevr_maps::PinnedKey& key : settings.configured_official_keys) {
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_official_key key_id=%s (from maps.official_keys, for %s)",
          nevr_maps::KeyIdText(key.key.key_id).c_str(), settings.config.base_url.c_str());
    }
    for (const nevr_maps::PinnedKey& key : settings.author_keys) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.MAPS] map_trusted_key key_id=%s (an author key from maps.trusted_keys: maps it signs at %s are trusted)",
          nevr_maps::KeyIdText(key.key.key_id).c_str(), settings.config.base_url.c_str());
    }
  }
  ch->config = settings.config;
  ch->config.dir = Slashes(Utf8(nevr_root::MapsCacheDir(root)));
  ch->config.is_server = is_server;
  ch->config.signed_only = nevr_plugin_trust::ActiveMode() == nevr_plugin_trust::Mode::SignedOnly;

  nevr_map_scripts::SetPolicy(is_server, AllowIds(ch->config.server_allow), ch->config.signed_only);
  nevr_resource_sets::SetMountObserver(&OnMount);
  const Registry registry{&RegisterSet, &ReportJsonText, &nevr_resource_sets::HoldsKey,
                          &nevr_resource_sets::WithdrawFileResourceSet};
  ChannelMounter mounter(registry);
  const std::string now = NowUtc();

  LocalScan local;
  if (local_present) {
    local = ScanLocal(*ch->local_env, *ch->fs, local_dir, local_dir + ".cache");
    if (!local.reason.empty()) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] LOCAL map_local reason=%s dir=%s", local.reason.c_str(),
          local_dir.c_str());
    }
  }
  std::vector<std::string> ids = ch->config.subscribed;
  for (const LocalMap& m : local.maps) ids.push_back(m.id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

  // One rule for an id in several places: EchoMapLoader's copy, then LOCAL, then the CDN.
  const LoaderFolders folders = LoaderFoldersThisRun(root, nevr_root::PluginsDir(root));
  const std::vector<std::pair<std::string, std::string>> loader = FindLoaderMaps(folders, ids);
  std::vector<std::string> shadowed;
  for (const auto& [id, path] : loader) {
    shadowed.push_back(id);
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_source id=%s winner=echomaploader path=%s (its LOCAL and CDN copies are skipped)",
        id.c_str(), path.c_str());
  }

  std::vector<std::string> expected;
  std::vector<std::pair<std::string, std::string>> local_view = loader;
  for (const LocalMap& m : local.maps) {
    if (std::find(shadowed.begin(), shadowed.end(), m.id) != shadowed.end()) continue;
    std::string token = m.reason;
    if (token.empty()) token = MountLocal(*ch->local_env, *ch->fs, m, mounter);
    if (token.empty()) {
      expected.push_back("map:" + m.id);
      local_view.emplace_back(m.id, m.path);
      Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] LOCAL map_mounted id=%s version=%s imported=%d file=%s", m.id.c_str(),
          m.package.version.c_str(), m.imported ? 1 : 0, m.path.c_str());
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] LOCAL map_skipped id=%s reason=%s file=%s", m.id.c_str(), token.c_str(),
          m.path.c_str());
    }
    if (std::find(ch->config.subscribed.begin(), ch->config.subscribed.end(), m.id) != ch->config.subscribed.end()) {
      shadowed.push_back(m.id);
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_source id=%s winner=local file=%s (the CDN copy is skipped)",
          m.id.c_str(), m.path.c_str());
    }
  }

  nevr_map_session_pin::ChannelView view;
  view.local = local_view;
  if (ch->cdn) {
    ch->config.local_maps = shadowed;
    const nevr_maps::BootReport report = nevr_maps::BootMount(*ch->env, *ch->fs, ch->config, mounter, ch->deriver.get(), now);
    ch->deferred = report.deferred;
    for (const std::string& id : report.mounted) expected.push_back("map:" + id);
    for (const nevr_maps::MountedMap& m : report.staged) ch->plan.pins.push_back(PinOf(m));
    view.listed = report.listed;
    view.subscribed = ch->config.subscribed;
    view.not_mounted = report.not_mounted;
    view.staged = ch->plan.pins;
  }
  ch->plan.expected = expected;
  ch->plan.registry = registry;
  g_channel = ch;

  if (mounter.AnyScript()) nevr_map_scripts::InstallMapScriptsRewriter();
  nevr_config_alias::InstallIfStaged();
  nevr_map_session_pin::SetChannelView(std::move(view));
  if (ch->cdn) nevr_map_session_pin::SetFetchRequest(&RequestFetch);
  nevr_config_alias_hook::SetMainLevelSource(&nevr_map_session_pin::SessionLevel);
  Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_start reason=ok cdn=%d local=%zu registered=%zu server=%d", ch->cdn ? 1 : 0,
      local.maps.size(), expected.size(), is_server ? 1 : 0);
}

}  // namespace

// The stock maps never wait on the channel: whatever goes wrong in it is logged and the boot goes on.
void Boot(std::uintptr_t game_base, bool is_server) {
  try {
    BootUnguarded(game_base, is_server);
  } catch (const std::exception& e) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MAPS] map_start reason=exception what=\"%s\" (nothing more is mounted or fetched this run)", e.what());
  }
}

void StartFetch() {
  Channel* ch = g_channel;
  if (ch == nullptr || !ch->cdn || ch->worker) return;
  if (!TryLockRoot(*ch->env, ch->config.dir)) {
    ch->busy = true;
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_start reason=busy (another game process is fetching into %s)",
        ch->config.dir.c_str());
    return;
  }
  ch->worker = std::make_unique<FetchWorker>(
      [ch](const std::function<bool()>& keep_going) {
        try {
          return nevr_maps::FetchAll(*ch->env, *ch->fs, ch->config, ch->deferred, ch->deriver.get(), NowUtc(),
                                     keep_going);
        } catch (const std::exception& e) {
          Log(EchoVR::LogLevel::Warning,
              "[NEVR.MAPS] map_done reason=exception what=\"%s\" (the cache keeps what it had; the next pass starts over)",
              e.what());
          throw;
        }
      },
      [ch] { return ch->env->NowMs(); });
  ch->worker->Start();
}

void StopFetch() {
  Channel* ch = g_channel;
  if (ch == nullptr || !ch->worker) return;
  if (!ch->worker->Stop(std::chrono::milliseconds(2000))) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_stop reason=join_timeout (the fetch ends at its next transfer bound)");
  }
}

void RequestFetch(const std::string& id) {
  Channel* ch = g_channel;
  if (ch == nullptr || !ch->cdn) return;
  if (std::find(ch->config.subscribed.begin(), ch->config.subscribed.end(), id) == ch->config.subscribed.end()) return;
  const char* reason = "queued";
  if (ch->busy) {
    reason = "busy";
  } else if (!ch->worker || !ch->worker->Request()) {
    reason = "not_now";
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_fetch_request id=%s reason=%s", id.c_str(), reason);
}

void Tick() {
  if (g_channel == nullptr) return;
  try {
    nevr_config_alias_hook::ReconcileIfFrameMoved();
    nevr_map_session_pin::Tick();
  } catch (const std::exception& e) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_tick reason=exception what=\"%s\" (logged once)", e.what());
    }
  }
}

}  // namespace nevr_map_channel
