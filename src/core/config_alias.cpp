#include "core/config_alias.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "nevr/symbol_hash.h"

#ifdef _WIN32
#include "core/logging.h"
#else
#include <cstdarg>
#include <cstdio>
#endif

namespace nevr_config_alias {

static_assert(kAliasRecordTypeWin10 == EchoVR::CSymbol64Hash("CJsonResourceWin10"));
static_assert(kAliasRecordTypeAndroid == EchoVR::CSymbol64Hash("CJsonResourceAndroid"));

namespace {
constexpr std::size_t kWarnLineBytes = 512;  // one log line; vsnprintf cuts a longer one

// One Warning line. The PC writes it through the runtime logger; the Quest sentinel defines PlatformWarn (config_alias.h).
void Warn(const char* format, ...) {
  char line[kWarnLineBytes];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof line, format, args);
  va_end(args);
#ifdef _WIN32
  Log(EchoVR::LogLevel::Warning, "%s", line);
#else
  PlatformWarn(line);
#endif
}
}  // namespace

namespace {

struct AllowedKey {
  std::uint64_t key;
  const char* name;
};
constexpr AllowedKey kAllowed[] = {{kArenaRulesKey, "mp_arena_rules"}};
static_assert(sizeof(kAllowed) / sizeof(kAllowed[0]) <= kMaxPerMap, "a snapshot holds one pair per allowed key");

const char* KeyName(std::uint64_t key) {
  for (const AllowedKey& allowed : kAllowed) {
    if (allowed.key == key) return allowed.name;
  }
  return "?";
}

void SortUnique(std::vector<std::uint64_t>* values) {
  std::sort(values->begin(), values->end());
  values->erase(std::unique(values->begin(), values->end()), values->end());
}

const Pair* FindPair(const std::vector<Pair>& pairs, std::uint64_t key) {
  for (const Pair& pair : pairs) {
    if (pair.key == key) return &pair;
  }
  return nullptr;
}

}  // namespace

bool KeyAllowed(std::uint64_t key) noexcept { return key == kArenaRulesKey; }

std::size_t Table::Stage(const std::string& owner, const std::string& id, std::uint64_t main_level,
                         const std::vector<Pair>& pairs) {
  Staged staged{id, main_level, {}};
  for (const Pair& pair : pairs) {
    if (staged.pairs.size() == kMaxPerMap) break;
    if (!KeyAllowed(pair.key) || pair.alias == 0 || pair.alias == pair.key) continue;
    if (FindPair(staged.pairs, pair.key) != nullptr) continue;
    staged.pairs.push_back(pair);
  }
  const std::size_t kept = staged.pairs.size();
  std::lock_guard<std::mutex> lock(mutex_);
  armed_.erase(owner);
  if (kept == 0) {
    staged_.erase(owner);
  } else {
    staged_[owner] = std::move(staged);
  }
  DropStaleLocked();
  generation_.fetch_add(1, std::memory_order_acq_rel);
  return kept;
}

void Table::Unstage(const std::string& owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  staged_.erase(owner);
  armed_.erase(owner);
  DropStaleLocked();
  generation_.fetch_add(1, std::memory_order_acq_rel);
}

bool Table::AnyStaged() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !staged_.empty();
}

std::size_t Table::Arm(const OwnerLevels& mounted_owner_levels, HoldsFn holds, void* ctx) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<std::string, Armed> armed;
  std::size_t armed_pairs = 0;
  for (const auto& owner_levels : mounted_owner_levels) {
    const auto staged = staged_.find(owner_levels.first);
    if (staged == staged_.end()) continue;
    Armed entry{staged->second.id, owner_levels.second, {}};
    for (const Pair& pair : staged->second.pairs) {
      if (holds != nullptr && holds(ctx, owner_levels.first, pair.alias)) {
        entry.pairs.push_back(pair);
      } else if (missing_logged_.emplace(owner_levels.first, pair.alias).second) {
        Warn(
            "[NEVR.MAPS] config_alias refused=alias_missing map=%s key=%s alias=0x%016llx (the map's mounted set does "
            "not hold the alias record, so the stock key is read)",
            entry.id.c_str(), KeyName(pair.key), static_cast<unsigned long long>(pair.alias));
      }
    }
    if (entry.pairs.empty()) continue;
    if (staged->second.main_level != 0) entry.levels.push_back(staged->second.main_level);
    entry.levels.erase(std::remove(entry.levels.begin(), entry.levels.end(), 0ull), entry.levels.end());
    SortUnique(&entry.levels);
    armed_pairs += entry.pairs.size();
    armed[owner_levels.first] = std::move(entry);
  }
  // Arming again with the same mount changes nothing: the generation stays, so a per-load record (the arena override's
  // line) is not repeated when the mount observer calls Arm twice for one load.
  const bool unchanged = armed == armed_;
  armed_ = std::move(armed);
  DropStaleLocked();
  if (!unchanged) generation_.fetch_add(1, std::memory_order_acq_rel);
  return armed_pairs;
}

void Table::DecideLocked(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels,
                         std::vector<Winner>* winners, std::vector<Loser>* losers) const {
  std::vector<std::uint64_t> loaded = loaded_levels;
  if (main_level != 0) loaded.push_back(main_level);
  SortUnique(&loaded);

  struct Candidate {
    const std::string* owner;
    const Armed* map;
    std::uint64_t alias;
    bool owns_main;
  };
  std::vector<Candidate> candidates;
  for (const AllowedKey& allowed : kAllowed) {
    candidates.clear();
    for (const auto& armed : armed_) {
      const Pair* pair = FindPair(armed.second.pairs, allowed.key);
      if (pair == nullptr) continue;
      const std::vector<std::uint64_t>& levels = armed.second.levels;
      const bool any_loaded = std::any_of(levels.begin(), levels.end(), [&](std::uint64_t level) {
        return std::binary_search(loaded.begin(), loaded.end(), level);
      });
      if (!any_loaded) continue;
      const bool owns_main = main_level != 0 && std::binary_search(levels.begin(), levels.end(), main_level);
      candidates.push_back({&armed.first, &armed.second, pair->alias, owns_main});
    }
    if (candidates.empty()) continue;
    const auto best = std::min_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      if (a.owns_main != b.owns_main) return a.owns_main;
      if (a.map->id != b.map->id) return a.map->id < b.map->id;
      return *a.owner < *b.owner;
    });
    winners->push_back({allowed.key, best->alias, *best->owner});
    if (losers == nullptr) continue;
    for (const Candidate& candidate : candidates) {
      if (&candidate != &*best) losers->push_back({allowed.key, best->map->id, candidate.map->id});
    }
  }
}

std::vector<Winner> Table::Decide(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Winner> winners;
  DecideLocked(main_level, loaded_levels, &winners, nullptr);
  return winners;
}

void Table::Reconcile(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Winner> winners;
  std::vector<Loser> losers;
  DecideLocked(main_level, loaded_levels, &winners, &losers);
  for (const Loser& loser : losers) {
    if (!conflicts_logged_.emplace(loser.key, loser.winner, loser.loser).second) continue;
    Warn(
        "[NEVR.MAPS] config_alias conflict key=%s winner=%s loser=%s (both maps are loaded and alias the key; the "
        "main level's map wins, else the lowest id, so the loser plays on the winner's values)",
        KeyName(loser.key), loser.winner.c_str(), loser.loser.c_str());
  }
  if (winners != published_) PublishLocked(winners);
}

void Table::PublishLocked(const std::vector<Winner>& winners) {
  // A key whose alias goes live (no winner for it was published, one is now) starts a new epoch: the arena override's
  // once-per-load line keys on it. A winner switch between two maps is not a new epoch.
  for (const Winner& winner : winners) {
    const bool was_live = std::any_of(published_.begin(), published_.end(),
                                      [&](const Winner& previous) { return previous.key == winner.key; });
    if (!was_live) ++live_epochs_[winner.key];
  }
  if (winners.empty()) {
    live_.store(nullptr, std::memory_order_release);
    published_.clear();
    return;
  }
  for (const auto& interned : interned_) {
    if (interned.first == winners) {
      live_.store(interned.second.get(), std::memory_order_release);
      published_ = winners;
      return;
    }
  }
  if (interned_.size() == kMaxSnapshots) {
    if (!cap_logged_) {
      cap_logged_ = true;
      Warn(
          "[NEVR.MAPS] config_alias reason=snapshot_cap snapshots=%zu (this process saw more combinations of loaded "
          "maps than it keeps; a new combination reads the stock keys)",
          kMaxSnapshots);
    }
    live_.store(nullptr, std::memory_order_release);
    published_.clear();
    return;
  }
  auto snapshot = std::make_unique<Snapshot>();
  for (const Winner& winner : winners) snapshot->pairs[snapshot->count++] = {winner.key, winner.alias};
  interned_.emplace_back(winners, std::move(snapshot));
  live_.store(interned_.back().second.get(), std::memory_order_release);
  published_ = winners;
}

// A published winner whose owner is no longer armed with that pair would answer with an alias its set may no longer
// hold: go back to the identity until the next Reconcile.
void Table::DropStaleLocked() {
  for (const Winner& winner : published_) {
    const auto armed = armed_.find(winner.owner);
    const Pair* pair = armed == armed_.end() ? nullptr : FindPair(armed->second.pairs, winner.key);
    if (pair == nullptr || pair->alias != winner.alias) {
      live_.store(nullptr, std::memory_order_release);
      published_.clear();
      return;
    }
  }
}

std::uint64_t Table::Resolve(std::uint64_t key) const noexcept {
  const Snapshot* snapshot = live_.load(std::memory_order_acquire);
  if (snapshot == nullptr) return key;
  for (std::size_t i = 0; i < snapshot->count; ++i) {
    if (snapshot->pairs[i].key == key) return snapshot->pairs[i].alias;
  }
  return key;
}

std::string Table::LiveMapId(std::uint64_t key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Winner& winner : published_) {
    if (winner.key != key) continue;
    const auto armed = armed_.find(winner.owner);
    return armed == armed_.end() ? std::string() : armed->second.id;
  }
  return std::string();
}

std::uint64_t Table::LiveEpoch(std::uint64_t key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = live_epochs_.find(key);
  return found == live_epochs_.end() ? 0 : found->second;
}

Table& Global() {
  static Table* const table = new Table();
  return *table;
}

std::size_t Stage(const std::string& owner, const std::string& id, std::uint64_t main_level,
                  const std::vector<Pair>& pairs) {
  return Global().Stage(owner, id, main_level, pairs);
}
void Unstage(const std::string& owner) { Global().Unstage(owner); }
std::size_t Arm(const OwnerLevels& mounted_owner_levels, HoldsFn holds, void* ctx) {
  return Global().Arm(mounted_owner_levels, holds, ctx);
}
std::vector<Winner> Decide(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels) {
  return Global().Decide(main_level, loaded_levels);
}
void Reconcile(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels) {
  Global().Reconcile(main_level, loaded_levels);
}
std::uint64_t Resolve(std::uint64_t key) noexcept { return Global().Resolve(key); }
std::string LiveMapId(std::uint64_t key) { return Global().LiveMapId(key); }
std::uint64_t LiveEpoch(std::uint64_t key) { return Global().LiveEpoch(key); }
void SetLiveRecord(const void* record) noexcept { Global().SetLiveRecord(record); }
const void* LiveRecord() noexcept { return Global().LiveRecord(); }

void LevelInstances::OnEnabled(const void* instance, std::uint64_t level) {
  if (instance != nullptr && level != 0) entries_[instance] = level;
}

void LevelInstances::OnDisabled(const void* instance) { entries_.erase(instance); }

std::vector<std::uint64_t> LevelInstances::Revalidate(std::uintptr_t session, std::uint64_t main_level, LiveFn live,
                                                      void* ctx) {
  // Leaving a value clears; arriving from none does not, or the levels enabled in a join's first frame are lost.
  const bool left_session = session_ != 0 && session != session_;
  const bool left_main_level = main_level_ != 0 && main_level != main_level_;
  if (left_session || left_main_level) entries_.clear();
  session_ = session;
  main_level_ = main_level;
  std::vector<std::uint64_t> levels;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (live != nullptr && live(ctx, it->first, it->second)) {
      levels.push_back(it->second);
      ++it;
    } else {
      it = entries_.erase(it);
    }
  }
  SortUnique(&levels);
  return levels;
}

}  // namespace nevr_config_alias
