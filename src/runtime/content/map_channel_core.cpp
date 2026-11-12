// The custom-map channel's PC pieces that need no game (map_channel.h, namespace detail): settings, the LOCAL source, the
// EchoMapLoader check, the Mounter, the mount observer's decisions, the root lock and the fetch worker.
#include <winsock2.h>
#include <windows.h>

#include "runtime/content/map_channel.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "core/config_alias.h"
#include "core/map_manifest.h"
#include "runtime/content/script_provider.h"

namespace nevr_map_channel {
namespace detail {

using nevr_maps::Grant;
using nevr_maps::PinnedKey;

namespace {

std::string Trim(const std::string& text) {
  const std::size_t from = text.find_first_not_of(" \t\r\n");
  if (from == std::string::npos) return std::string();
  const std::size_t to = text.find_last_not_of(" \t\r\n");
  return text.substr(from, to - from + 1);
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

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

std::string NowUtc() {
  SYSTEMTIME t{};
  GetSystemTime(&t);
  char text[24];
  std::snprintf(text, sizeof text, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                t.wSecond);
  return text;
}

std::vector<std::string> SplitCsv(const std::string& text) {
  std::vector<std::string> out;
  std::size_t from = 0;
  while (from <= text.size()) {
    const std::size_t comma = std::min(text.find(',', from), text.size());
    std::string item = Trim(text.substr(from, comma - from));
    if (!item.empty()) out.push_back(std::move(item));
    from = comma + 1;
  }
  return out;
}

bool ParseKeyText(const std::string& text, PinnedKey* out) {
  std::size_t from = 0;
  while (from < text.size()) {
    std::size_t end = text.find('\n', from);
    if (end == std::string::npos) end = text.size();
    const std::string line = Trim(text.substr(from, end - from));
    from = end + 1;
    if (line.empty() || line.rfind("untrusted comment:", 0) == 0) continue;
    *out = PinnedKey{};
    return nevr_minisign::ParsePublicKey(line, &out->key);
  }
  return false;
}

namespace {

bool ReadKeyList(const std::string& csv_text, const char* config_key, std::vector<PinnedKey>* keys, Settings* s) {
  const std::vector<std::string> lines = SplitCsv(csv_text);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    PinnedKey key;
    if (!ParseKeyText(lines[i], &key)) {
      s->reason = "bad_key";
      s->detail = std::string(config_key) + "[" + std::to_string(i) + "]";
      return false;
    }
    keys->push_back(key);
  }
  return true;
}

}  // namespace

Settings ReadSettings(const Lookup& value, const Lookup& csv, const std::vector<PinnedKey>& compiled_official) {
  Settings s;
  if (!ReadKeyList(csv("maps_official_keys"), "maps.official_keys", &s.configured_official_keys, &s) ||
      !ReadKeyList(csv("maps_trusted_keys"), "maps.trusted_keys", &s.author_keys, &s)) {
    return s;
  }
  std::vector<PinnedKey> official = compiled_official;
  official.insert(official.end(), s.configured_official_keys.begin(), s.configured_official_keys.end());
  if (!official.empty() && !s.author_keys.empty()) {
    s.reason = "mixed_keys";
    s.detail = "official and author keys are never trusted for one base_url";
    return s;
  }

  nevr_maps::ChannelConfig& c = s.config;
  c.subscribed = SplitCsv(csv("maps_subscribed"));
  std::sort(c.subscribed.begin(), c.subscribed.end());
  c.subscribed.erase(std::unique(c.subscribed.begin(), c.subscribed.end()), c.subscribed.end());
  c.base_url = Trim(value("maps_base_url"));
  if (!c.base_url.empty() && c.base_url.back() != '/') c.base_url += '/';
  c.server_allow = SplitCsv(csv("maps_server_allow"));
  c.channel = Trim(value("maps_channel"));
  c.boot_budget_ms = static_cast<std::uint32_t>(std::strtoul(value("maps_boot_budget_ms").c_str(), nullptr, 10));
  const unsigned long long cap_mib = std::strtoull(value("maps_cap_mib").c_str(), nullptr, 10);
  if (cap_mib > 0 && cap_mib < (1ull << 40)) c.cap_bytes = cap_mib << 20;
  c.keys = official;
  for (PinnedKey& key : c.keys) key.official = true;
  c.keys.insert(c.keys.end(), s.author_keys.begin(), s.author_keys.end());
  if (c.subscribed.empty()) s.reason = "not_configured";
  return s;
}

std::vector<std::pair<std::string, std::string>> FindLoaderMaps(const LoaderFolders& folders,
                                                                 const std::vector<std::string>& ids) {
  std::vector<std::pair<std::string, std::string>> found;
  if (folders.plugin_dll.empty() || folders.loader_root.empty() || !Exists(folders.plugin_dll)) return found;
  const std::wstring maps_dir = folders.loader_root + (folders.testmap ? L"\\maps\\.testmap" : L"\\maps");
  for (const std::string& id : ids) {
    const std::wstring path = maps_dir + L"\\" + Wide(id) + L".kfclvl";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      found.emplace_back(id, Utf8(path));
    }
  }
  return found;
}

LoaderFolders LoaderFoldersThisRun(const std::wstring& nevr_root, const std::wstring& plugins_dir) {
  const auto env = [](const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
  };
  LoaderFolders folders;
  folders.plugin_dll = plugins_dir + L"\\EchoMapLoader.dll";
  folders.loader_root = env(L"ECHOMAPS_ROOT");
  if (folders.loader_root.empty()) folders.loader_root = nevr_root + L"\\echomaps";
  while (!folders.loader_root.empty() && (folders.loader_root.back() == L'\\' || folders.loader_root.back() == L'/')) {
    folders.loader_root.pop_back();
  }
  folders.testmap = env(L"ECHOMAPS_TESTMAP") == L"1";
  return folders;
}

Grant ChannelMounter::Script(const std::string& owner, std::uint64_t hash, std::int64_t held,
                             const std::string& sha256) {
  // The boot stamped the file against the stamp the fetch recorded after hashing it, so the provider does not rehash.
  const nevr_map_scripts::Outcome o = nevr_map_scripts::Register(owner, hash, held, sha256, false);
  if (o.ok()) any_script_ = true;
  return Grant{o.ok(), o.reason};
}

Grant ChannelMounter::Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                            const std::string& stock_sha256) {
  const nevr_map_scripts::Outcome o = nevr_map_scripts::RegisterRekey(owner, hash, stock_source, stock_sha256);
  if (o.ok()) any_script_ = true;
  return Grant{o.ok(), o.reason};
}

Grant ChannelMounter::Set(const std::string& owner, std::int64_t held, const std::string& label,
                          const std::string& sha256, bool) {
  if (registry_.register_set == nullptr) return Grant{false, "no_registry"};
  return registry_.register_set(owner, held, label, sha256);
}

Grant ChannelMounter::StageAliases(const std::string& owner, const std::string& id, std::uint64_t main_level,
                                   const std::vector<nevr_maps::ConfigAlias>& aliases) {
  if (aliases.empty()) return Grant{true, ""};
  std::vector<nevr_config_alias::Pair> pairs;
  pairs.reserve(aliases.size());
  for (const nevr_maps::ConfigAlias& a : aliases) pairs.push_back({a.key, a.alias});
  const std::size_t staged = nevr_config_alias::Stage(owner, id, main_level, pairs);
  return staged == pairs.size() ? Grant{true, ""} : Grant{false, "alias_partial"};
}

// The boot stamped the rebuilt file, but the provider rehashes it anyway: it is the recipe's result, and a script is
// tens of KiB.
Grant ChannelMounter::DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held,
                                    const std::string& sha256, const std::string& stock_sha256) {
  const nevr_map_scripts::Outcome o = nevr_map_scripts::RegisterDerived(owner, hash, held, sha256, stock_sha256);
  if (o.ok()) any_script_ = true;
  return Grant{o.ok(), o.reason};
}

void ChannelMounter::Withdraw(const std::string& owner) {
  nevr_map_scripts::Withdraw(owner);
  nevr_config_alias::Unstage(owner);
  if (registry_.withdraw_set != nullptr) registry_.withdraw_set(owner);
}

std::vector<std::pair<std::string, std::vector<std::uint64_t>>> MountedMapOwners(const std::string& report_json) {
  std::vector<std::pair<std::string, std::vector<std::uint64_t>>> owners;
  const nlohmann::json doc = nlohmann::json::parse(report_json, nullptr, false);
  if (!doc.is_array()) return owners;
  for (const nlohmann::json& set : doc) {
    if (!set.is_object()) continue;
    const auto plugin = set.find("plugin");
    if (plugin == set.end() || !plugin->is_string()) continue;
    const std::string owner = plugin->get<std::string>();
    if (owner.rfind("map:", 0) != 0) continue;
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

namespace {
struct HoldsContext {
  const std::set<std::string>* owners;
  bool (*holds_key)(std::uint64_t type, std::uint64_t name);
};
}  // namespace

// The sets are disjoint (kCollides), so a mounted owner's set is the holder of a key some mounted set holds.
bool HoldsAlias(void* ctx, const std::string& owner, std::uint64_t alias) {
  const HoldsContext* c = static_cast<const HoldsContext*>(ctx);
  return c != nullptr && c->holds_key != nullptr && c->owners->count(owner) != 0 && c->holds_key(kAliasRecordType, alias);
}

MountOutcome ApplyMount(const MountPlan& plan, const std::string& report_json) {
  MountOutcome out;
  std::set<std::string> expected(plan.expected.begin(), plan.expected.end());
  nevr_config_alias::OwnerLevels owner_levels;
  std::set<std::string> mounted;
  for (auto& [owner, levels] : MountedMapOwners(report_json)) {
    if (expected.count(owner) == 0 || !mounted.insert(owner).second) continue;
    owner_levels.emplace_back(owner, std::move(levels));
  }
  for (const std::string& owner : expected) {
    if (mounted.count(owner) != 0) {
      out.mounted.push_back(owner);
      continue;
    }
    nevr_map_scripts::Withdraw(owner);
    nevr_config_alias::Unstage(owner);
    out.withdrawn.push_back(owner);
  }
  HoldsContext ctx{&mounted, plan.registry.holds_key};
  out.armed = nevr_config_alias::Arm(owner_levels, &HoldsAlias, &ctx);
  std::vector<nevr_map_session_pin::MapPin> pins;
  for (const nevr_map_session_pin::MapPin& pin : plan.pins) {
    if (mounted.count("map:" + pin.id) != 0) pins.push_back(pin);
  }
  nevr_map_session_pin::SetMountedPins(std::move(pins));
  return out;
}

bool TryLockRoot(nevr_cargo::Env& env, const std::string& root) {
  std::unique_ptr<nevr_cargo::Hasher> hasher = env.NewHasher();
  hasher->Update(root.data(), root.size());
  const std::wstring name = L"Local\\nevr-maps-" + Wide(hasher->Hex().substr(0, 16));
  // The name, not ownership, is the lock: it exists while any process keeps a handle, and this one never closes its
  // handle, so the boot thread ending does not free it (an owned mutex would be abandoned and taken again).
  HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
  if (mutex == nullptr) return false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return false;
  }
  return true;
}

void FetchWorker::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_ || stop_) return;
  started_ = true;
  thread_ = std::thread(&FetchWorker::Run, this);
}

void FetchWorker::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    lock.unlock();
    pass_([this] { return keep_going_.load(); });
    passes_.fetch_add(1);
    lock.lock();
    wake_.wait(lock, [this] { return stop_ || requested_; });
    requested_ = false;
  }
  finished_ = true;
  wake_.notify_all();
}

bool FetchWorker::Request() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || stop_) return false;
  const std::uint64_t now = now_ms_();
  if (any_extra_ && now - last_extra_ms_ < kExtraFetchGapMs) return false;
  any_extra_ = true;
  last_extra_ms_ = now;
  requested_ = true;
  wake_.notify_all();
  return true;
}

bool FetchWorker::Stop(std::chrono::milliseconds wait) {
  std::unique_lock<std::mutex> lock(mutex_);
  // A second caller (two shutdown paths at once) only waits: the first one joins or detaches thread_.
  if (stop_) return wake_.wait_for(lock, wait, [this] { return !started_ || finished_; });
  stop_ = true;
  keep_going_ = false;
  wake_.notify_all();
  if (!thread_.joinable()) return !started_ || finished_;
  const bool done = wake_.wait_for(lock, wait, [this] { return finished_; });
  lock.unlock();
  if (done) {
    thread_.join();
  } else {
    thread_.detach();
  }
  return done;
}

}  // namespace detail
}  // namespace nevr_map_channel
