#pragma once
// ConfigAlias (docs/adr/0047, decision 5): a mounted map's balance-config keys answered from its own
// record (mp_arena_rules -> <id>_arena_rules) while ANY level of that map is loaded. This half is pure: the staged and
// armed tables, the winner rule, and the interned snapshots the lookup detour (runtime/content/config_alias_hook.h)
// reads. Nothing here touches the game.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace nevr_config_alias {

constexpr std::size_t kMaxPerMap = 4, kMaxSnapshots = 64;

// rad_hash("mp_arena_rules"). v1 aliases only this key (ADR 0047 decision 5); adding one is an allowlist edit.
constexpr std::uint64_t kArenaRulesKey = 0x12220d7a60d8f9c3ull;

// The type of the <id>_arena_rules record a map's set carries, the type stock mp_arena_rules has in both platforms'
// manifests (48037dc70b0ecab2): a CJsonResource. A CJsonConfigCR is only a level's registry of those, never the record.
constexpr std::uint64_t kAliasRecordTypeWin10 = 0x607d858c90268f9aull;    // CJsonResourceWin10
constexpr std::uint64_t kAliasRecordTypeAndroid = 0x921e9761b45fc6e4ull;  // CJsonResourceAndroid

bool KeyAllowed(std::uint64_t key) noexcept;

struct Pair {
  std::uint64_t key = 0, alias = 0;
  bool operator==(const Pair& o) const { return key == o.key && alias == o.alias; }
  bool operator!=(const Pair& o) const { return !(*this == o); }
};

struct Winner {
  std::uint64_t key = 0, alias = 0;
  std::string owner;
  bool operator==(const Winner& o) const { return key == o.key && alias == o.alias && owner == o.owner; }
  bool operator!=(const Winner& o) const { return !(*this == o); }
};

using HoldsFn = bool (*)(void* ctx, const std::string& owner, std::uint64_t alias);
using OwnerLevels = std::vector<std::pair<std::string, std::vector<std::uint64_t>>>;

// One table of staged and armed maps. The free functions below drive the process's one table; tests build their own.
class Table {
 public:
  Table() = default;
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Boot thread, before the mount. Keeps the allowed pairs with alias != key and alias != 0, one per key, at most
  // kMaxPerMap, and returns how many it kept (fewer than pairs.size() is the mount's alias_partial). Nothing is live.
  // Staging an owner again replaces its pairs and disarms it.
  std::size_t Stage(const std::string& owner, const std::string& id, std::uint64_t main_level,
                    const std::vector<Pair>& pairs);
  void Unstage(const std::string& owner);
  bool AnyStaged() const;

  // Game thread, from the mount observer. The armed owners become exactly the staged owners in the list; each keeps
  // the pairs `holds` confirms (`config_alias refused=alias_missing` for the rest), with its levels (Set::levels).
  // Returns the number of armed pairs. Until an owner is armed, Resolve never answers with its alias.
  std::size_t Arm(const OwnerLevels& mounted_owner_levels, HoldsFn holds, void* ctx);

  // The winners for one load state (ADR 0047 decision 5): per allowed key, among the armed maps with a level in
  // `loaded_levels` or equal to `main_level`, the map that owns the main level, else the lowest map id. Sorted by key.
  std::vector<Winner> Decide(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels) const;

  // Publishes the interned snapshot for this state and logs each new (key, winner, loser) once.
  void Reconcile(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels);

  // Any thread: one acquire load and at most kMaxPerMap compares. No lock, no allocation.
  std::uint64_t Resolve(std::uint64_t key) const noexcept;

  // Bumped by every Stage, Unstage and Arm, so a caller that skips Reconcile while its inputs are unchanged still
  // reconciles after the armed maps change.
  std::uint64_t Generation() const noexcept { return generation_.load(std::memory_order_acquire); }

 private:
  struct Snapshot {
    std::size_t count = 0;
    Pair pairs[kMaxPerMap];
  };
  struct Staged {
    std::string id;
    std::uint64_t main_level = 0;
    std::vector<Pair> pairs;
  };
  struct Armed {
    std::string id;
    std::vector<std::uint64_t> levels;  // sorted, the main level included
    std::vector<Pair> pairs;
  };
  struct Loser {
    std::uint64_t key;
    std::string winner, loser;  // map ids
  };

  void DecideLocked(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels,
                    std::vector<Winner>* winners, std::vector<Loser>* losers) const;
  void PublishLocked(const std::vector<Winner>& winners);
  void DropStaleLocked();

  mutable std::mutex mutex_;
  std::map<std::string, Staged> staged_;
  std::map<std::string, Armed> armed_;
  std::vector<Winner> published_;
  std::vector<std::pair<std::vector<Winner>, std::unique_ptr<Snapshot>>> interned_;
  std::set<std::tuple<std::uint64_t, std::string, std::string>> conflicts_logged_;
  std::set<std::pair<std::string, std::uint64_t>> missing_logged_;
  bool cap_logged_ = false;
  std::atomic<const Snapshot*> live_{nullptr};
  std::atomic<std::uint64_t> generation_{0};
};

// The process's table. It is never destroyed, so a lookup on a game thread at exit still reads a live snapshot.
Table& Global();

std::size_t Stage(const std::string& owner, const std::string& id, std::uint64_t main_level,
                  const std::vector<Pair>& pairs);
void Unstage(const std::string& owner);
// Boot thread: the lookup hook goes in only when something is staged. Defined in runtime/content/config_alias_hook.cpp,
// because the hook is the runtime's.
bool InstallIfStaged();
std::size_t Arm(const OwnerLevels& mounted_owner_levels, HoldsFn holds, void* ctx);
std::vector<Winner> Decide(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels);
void Reconcile(std::uint64_t main_level, const std::vector<std::uint64_t>& loaded_levels);
std::uint64_t Resolve(std::uint64_t key) noexcept;

#ifndef _WIN32
// The Quest sentinel defines it (quest/sentinel/config_alias_hook.cpp): one Warning line from the table.
void PlatformWarn(const char* line) noexcept;
#endif

// The second source of the loaded levels, the sublevels: the (instance, level) pairs a level-enable detour saw, kept
// as a set and revalidated against game state on every read, so an event alone never keeps an entry.
class LevelInstances {
 public:
  using LiveFn = bool (*)(void* ctx, const void* instance, std::uint64_t level);
  void OnEnabled(const void* instance, std::uint64_t level);
  void OnDisabled(const void* instance);
  // Clears the set when `session` or `main_level` moves off a non-zero value it had at the last call, drops every
  // entry `live` rejects, and returns the levels left, sorted and unique.
  std::vector<std::uint64_t> Revalidate(std::uintptr_t session, std::uint64_t main_level, LiveFn live, void* ctx);
  std::size_t size() const { return entries_.size(); }

 private:
  std::map<const void*, std::uint64_t> entries_;
  std::uintptr_t session_ = 0;
  std::uint64_t main_level_ = 0;
};

}  // namespace nevr_config_alias
