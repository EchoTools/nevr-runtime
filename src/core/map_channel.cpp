// The custom-map channel's start (map_channel.h): the settings rules, the start decision, the source ordering, the boot mount, the
// background pass and the worker thread. Platform-free: no Win32, no POSIX, no Log(); every record goes through Env::Event.
#include "core/map_channel.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <utility>

namespace nevr_map_channel {

namespace {

std::string Trim(const std::string& text) {
  const std::size_t from = text.find_first_not_of(" \t\r\n");
  if (from == std::string::npos) return std::string();
  const std::size_t to = text.find_last_not_of(" \t\r\n");
  return text.substr(from, to - from + 1);
}

// One key from one item of a list. The item may be a whole .pub file (its untrusted comment line and its key line).
bool ParseKeyItem(const std::string& text, nevr_maps::PinnedKey* out) {
  *out = nevr_maps::PinnedKey{};
  return nevr_minisign::ParsePublicKeyText(text, &out->key);
}

// Reads one key list into `keys` (official-trust, never compiled, or author keys). False with the refusal in `s` on a key that
// does not parse.
bool ReadKeys(const std::vector<std::string>& items, const char* config_key, bool official, std::vector<nevr_maps::PinnedKey>* keys,
              Settings* s) {
  std::size_t index = 0;
  for (const std::string& raw : items) {
    const std::string item = Trim(raw);
    if (item.empty()) continue;
    nevr_maps::PinnedKey key;
    if (!ParseKeyItem(item, &key)) {
      s->reason = "bad_key";
      s->detail = std::string(config_key) + "[" + std::to_string(index) + "]";
      return false;
    }
    key.official = official;
    key.compiled = false;
    keys->push_back(key);
    ++index;
  }
  return true;
}

}  // namespace

const std::vector<nevr_maps::PinnedKey>& CompiledOfficialKeys() {
  static const std::vector<nevr_maps::PinnedKey> keys;  // ADR 0048 decision 12: none until the official key is chosen
  return keys;
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

Settings ReadSettings(const ScalarLookup& value, const ListLookup& list, const std::vector<nevr_maps::PinnedKey>& compiled) {
  Settings s;
  std::string base = Trim(value("maps_base_url"));
  if (base.empty()) {
    s.reason = "not_configured";  // nothing else is read: no key is parsed, no limit is taken
    return s;
  }
  if (base.back() != '/') base += '/';
  s.base_url = base;
  s.channel = Trim(value("maps_channel"));
  s.boot_budget_ms = static_cast<std::uint32_t>(std::strtoul(value("maps_boot_budget_ms").c_str(), nullptr, 10));

  std::vector<nevr_maps::PinnedKey> configured_official, author;
  if (!ReadKeys(list("maps_official_keys"), "maps.official_keys", true, &configured_official, &s) ||
      !ReadKeys(list("maps_trusted_keys"), "maps.trusted_keys", false, &author, &s)) {
    return s;
  }
  const bool any_official = !compiled.empty() || !configured_official.empty();
  if (any_official && !author.empty()) {
    s.reason = "mixed_keys";
    s.detail = "official and author keys are never trusted for one base_url (ADR 0048 decision 3)";
    return s;
  }
  for (const nevr_maps::PinnedKey& key : compiled) {
    nevr_maps::PinnedKey k = key;
    k.official = true;
    k.compiled = true;
    s.keys.push_back(k);
  }
  s.keys.insert(s.keys.end(), configured_official.begin(), configured_official.end());
  s.keys.insert(s.keys.end(), author.begin(), author.end());
  return s;
}

Start DecideStart(const Settings& settings) {
  if (settings.reason == "not_configured") return Start::kOff;
  return settings.reason.empty() ? Start::kRun : Start::kRefused;
}

std::vector<std::string> FetchGroups(const std::vector<std::string>& recorded) {
  std::vector<std::string> groups;
  for (const std::string& group : recorded) {
    if (!nevr_maps::IsPathSafeGroupId(group)) continue;
    if (std::find(groups.begin(), groups.end(), group) != groups.end()) continue;
    groups.push_back(group);
  }
  return groups;
}

std::vector<std::string> LocalShadowIds(const std::vector<std::string>& local_ids) {
  std::set<std::string> sorted(local_ids.begin(), local_ids.end());
  return std::vector<std::string>(sorted.begin(), sorted.end());
}

Seams NoSeams() { return Seams{}; }

MountDecision DecideMount(const std::vector<std::string>& expected, const nevr_config_alias::OwnerLevels& mounted_owners,
                          const std::function<void(const std::string& owner)>& withdraw,
                          const std::function<bool(const std::string& owner, std::uint64_t alias)>& holds_alias) {
  MountDecision out;
  std::set<std::string> mounted;
  nevr_config_alias::OwnerLevels owner_levels;
  for (const auto& [owner, levels] : mounted_owners) {
    if (std::find(expected.begin(), expected.end(), owner) == expected.end() || !mounted.insert(owner).second) continue;
    owner_levels.emplace_back(owner, levels);
  }
  for (const std::string& owner : expected) {
    if (mounted.count(owner) != 0) {
      out.mounted.push_back(owner);
      continue;
    }
    withdraw(owner);
    out.withdrawn.push_back(owner);
  }
  // Arm asks through a plain function pointer: the context carries the mounted set and the platform's answer.
  struct Context {
    const std::set<std::string>* mounted;
    const std::function<bool(const std::string&, std::uint64_t)>* holds;
  };
  Context context{&mounted, &holds_alias};
  out.armed = nevr_config_alias::Arm(
      owner_levels,
      [](void* ctx, const std::string& owner, std::uint64_t alias) {
        const Context* c = static_cast<const Context*>(ctx);
        return c->mounted->count(owner) != 0 && (*c->holds)(owner, alias);
      },
      &context);
  return out;
}

Channel::Channel(nevr_maps::ChannelConfig config, const Settings& settings) : config_(std::move(config)) {
  config_.base_url = settings.base_url;
  config_.channel = settings.channel;
  config_.boot_budget_ms = settings.boot_budget_ms;
  config_.keys = settings.keys;
  refusal_ = settings.reason;
  refusal_detail_ = settings.detail;
  start_ = DecideStart(settings);
}

const BootResult& Channel::Boot(nevr_cargo::Env& env, nevr_maps::MapFs& fs, nevr_maps::Mounter& mounter,
                                nevr_maps::Deriver* deriver, const std::string& now_utc,
                                const std::vector<std::string>& local_ids) {
  if (booted_) return result_;  // the second call answers the first: no record, no mount
  booted_ = true;
  result_.start = start_;
  if (start_ == Start::kOff) {
    result_.reason = "not_configured";
    env.Event(nevr_cargo::EventLevel::kInfo, "map_start", {{"reason", "not_configured"}});
    return result_;
  }
  if (start_ == Start::kRefused) {
    result_.reason = refusal_;
    env.Event(nevr_cargo::EventLevel::kWarn, "map_start",
              {{"reason", refusal_}, {"detail", refusal_detail_}, {"consequence", "no map is mounted or fetched this run"}});
    return result_;
  }
  config_.local_maps = LocalShadowIds(local_ids);
  // The cache index's groups, latest first: the mount reads them, and hashes nothing it has stamped (decision 4, ADR 0045 decision 7).
  nevr_maps::CacheMount mount(env, fs, config_, mounter, deriver);
  result_.report = mount.Mount(nevr_maps::CachedGroupIds(env, config_.dir), now_utc);
  result_.reason = "ok";
  env.Event(nevr_cargo::EventLevel::kInfo, "map_start",
            {{"reason", "ok"},
             {"mounted", result_.report.mounted.size()},
             {"deferred", result_.report.deferred.size()},
             {"local", config_.local_maps.size()}});
  return result_;
}

std::vector<nevr_maps::FetchReport> Channel::FetchPass(nevr_cargo::Env& env, nevr_maps::MapFs& fs, nevr_maps::Deriver* deriver,
                                                       const std::vector<std::string>& groups, const std::string& now_utc,
                                                       const std::function<bool()>& keep_going) {
  std::vector<nevr_maps::FetchReport> reports;
  if (start_ != Start::kRun || !booted_) return reports;
  for (const std::string& group : groups) {
    if (!keep_going()) break;
    reports.push_back(nevr_maps::FetchGroup(env, fs, config_, group, result_.report.deferred, deriver, now_utc, keep_going));
  }
  return reports;
}

// ---- FetchWorker -------------------------------------------------------------------------------------------------------------------

void FetchWorker::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_ || stop_) return;
  started_ = true;
  thread_ = std::thread(&FetchWorker::Run, this);
}

bool FetchWorker::started() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return started_;
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

}  // namespace nevr_map_channel
