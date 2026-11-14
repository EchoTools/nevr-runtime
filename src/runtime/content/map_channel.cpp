// The custom-map channel's PC start (map_channel.h): the Win32 seams and the hooks the core's decisions allow. Every decision is the
// core's (core/map_channel.h). Written against the Win32 API and not built or run under the Quest-only verification order.
#include <winsock2.h>
#include <windows.h>

#include "runtime/content/map_channel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cargo_env.h"
#include "core/config_alias.h"
#include "core/logging.h"
#include "core/map_channel.h"
#include "core/map_manifest.h"
#include "core/nevr_root.h"
#include "core/resource_set_registry.h"
#include "runtime/cargo/stock_source.h"
#include "runtime/cargo/win_env.h"
#include "runtime/content/script_provider.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/lifecycle/game_image_guard.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/resource_sets.h"

namespace nevr_map_channel::pc {
namespace {

constexpr const char* kEnvTag = "[NEVR.MAPS]";  // the tag the Win env writes its records under
constexpr std::size_t kLockNameHexDigits = 16;  // the root lock's name: the first digits of sha256(root)
constexpr std::uint32_t kStopWaitMs = 2000;     // the fetch's stop waits this long, then detaches (the fetch ends at its next transfer)

// The run's channel, heap-owned and never freed: the worker and the mount observer use it for the process's life.
struct State {
  std::unique_ptr<nevr_cargo::Env> env;
  std::unique_ptr<nevr_maps::MapFs> fs;
  std::unique_ptr<nevr_maps::StockSource> stock;
  std::unique_ptr<nevr_maps::ChannelDeriver> deriver;
  std::unique_ptr<Channel> channel;
  std::unique_ptr<FetchWorker> worker;
  std::vector<std::string> granted;  // the owners this run's boot granted ("map:<id>")
};
State* g_state = nullptr;

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
                         const std::string& stock_sha256) override {
    return Took(nevr_map_scripts::RegisterRekey(owner, hash, stock_source, stock_sha256));
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
  const MountDecision decision = DecideMount(
      st->granted, MountedOwners(nevr_resource_sets::ReportJson()),
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
bool TryLockRoot(nevr_cargo::Env& env, const std::string& root) {
  std::unique_ptr<nevr_cargo::Hasher> hasher = env.NewHasher();
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

void BootUnguarded(std::uintptr_t game_base, bool is_server) {
  if (is_server) {
    // The server's group fetch and its join hold are a later part of the channel (ADR 0048 decisions 13 and 17); until then a
    // dedicated server runs without the channel, and says so once.
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_start reason=server_not_served (the server's group fetch is a later part)");
    return;
  }
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
  st->env = nevr_cargo::MakeWinEnv(kEnvTag);
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
    nevr_cargo::DeviceFacts facts;
    st->env->QueryDevice(config.dir, &facts);
    nevr_map_scripts::SetPolicy(config.signed_only, nevr_maps::MapDocumentBytes(facts));
  }

  PcMounter mounter;
  const Seams seams = NoSeams();
  const BootResult& boot = st->channel->Boot(*st->env, *st->fs, mounter, st->deriver.get(), NowUtc(), Ask(seams.local_map_ids));
  if (boot.start != Start::kRun) return;  // the core wrote why (not_configured, or the refusal)

  for (const std::string& id : boot.report.mounted) st->granted.push_back(std::string(nevr_resource_sets::kMapOwnerPrefix) + id);
  if (mounter.any_script()) nevr_map_scripts::InstallMapScriptsRewriter();
  nevr_resource_sets::SetMountObserver(&OnMount);
  nevr_config_alias::InstallIfStaged();
}

void StartFetchUnguarded() {
  State* const st = g_state;
  if (st == nullptr || st->channel->start() != Start::kRun || st->worker) return;
  const std::string dir = st->channel->config().dir;
  if (!TryLockRoot(*st->env, dir)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] map_start reason=busy (another game process is fetching into %s)", dir.c_str());
    return;
  }
  st->worker = std::make_unique<FetchWorker>(
      [st](const std::function<bool()>& keep_going) {
        try {
          const Seams seams = NoSeams();
          st->channel->FetchPass(*st->env, *st->fs, st->deriver.get(), FetchGroups(Ask(seams.recorded_groups)), NowUtc(),
                                 keep_going);
        } catch (const std::exception& e) {
          Log(EchoVR::LogLevel::Warning,
              "[NEVR.MAPS] map_done reason=exception what=\"%s\" (the cache keeps what it had; the next pass starts over)", e.what());
        }
      },
      [st] { return st->env->NowMs(); });
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
  if (st == nullptr || !st->worker) return;
  if (!st->worker->Stop(std::chrono::milliseconds(kStopWaitMs))) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] map_stop reason=join_timeout (the fetch ends at its next transfer bound)");
  }
}

}  // namespace nevr_map_channel::pc
