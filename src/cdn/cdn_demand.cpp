#include "cdn/cdn_demand.h"

#include <algorithm>
#include <map>
#include <set>

#include "core/combat_level_class.h"
#include "cdn/cdn_format.h"
#include "cdn/cdn_prefetch.h"
#include "cdn/cdn_store.h"

namespace nevr_cdn {

namespace {

constexpr std::size_t kU64Bytes = sizeof(std::uint64_t);
constexpr std::size_t kAssetRecordBytes = 2 * kU64Bytes;      // an N record: {u64, u64}
constexpr std::size_t kBlobRecordBytes = kU64Bytes + 2 * kU32Bytes;  // an M record: {u64, u32, u32}
constexpr std::size_t kBitsPerByte = 8;
constexpr std::size_t kFirstByte = 0;

constexpr const char* kNoMode = "no_mode";
constexpr const char* kArchiveUnavailable = "archive_unavailable";
constexpr const char* kBadArchive = "bad_archive";
constexpr const char* kOverStorage = "over_storage";
constexpr const char* kDeadline = "deadline";
constexpr const char* kCancelled = "cancelled";
constexpr const char* kLimitsRefused = "limits";
constexpr const char* kCacheDir = "cache_dir";

using Key = std::pair<std::uint64_t, std::uint64_t>;  // (type, name)

// Where a needed object is had from (places 1 to 3).
enum class Place : std::uint8_t { kCache, kInstalled, kCdn };

std::uint64_t ReadLE(const std::vector<std::uint8_t>& body, std::size_t at, std::size_t bytes) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(body[at + i]) << (i * kBitsPerByte);
  return v;
}

// The body of an archive as the game lists it, in the bytes the store or the CDN hold.
std::vector<std::uint8_t> BytesOf(const std::string& text) {
  std::vector<std::uint8_t> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::uint8_t>(text[i]);
  return out;
}

// The service's game types and what each takes (PLAN-v7 R6-a-2a, the table of R6-a-2a 1.3). One row per game type of
// nevr_game_type_table::kGameTypes; a row's levels are the class lists its kind names in LevelsOfMode.
enum class ModeKind : std::uint8_t { kArena, kCombat, kSocial, kSocialNpe };
struct ModeRow {
  const char* game_type;
  ModeKind kind;
};
inline constexpr ModeRow kModeRows[] = {
    {"echo_arena", ModeKind::kArena},
    {"echo_arena_private", ModeKind::kArena},
    {"echo_arena_tournament", ModeKind::kArena},
    {"echo_arena_public_ai", ModeKind::kArena},
    {"echo_arena_practice", ModeKind::kArena},
    {"echo_combat", ModeKind::kCombat},
    {"echo_combat_private", ModeKind::kCombat},
    {"echo_combat_tournament", ModeKind::kCombat},
    {"social_2.0", ModeKind::kSocial},
    {"social_2.0_private", ModeKind::kSocial},
    {"social_2.0_npe", ModeKind::kSocialNpe},
};

// The levels of one kind, added to `out`.
template <std::size_t N>
void AddAll(const std::uint64_t (&levels)[N], std::set<std::uint64_t>* out) {
  for (const std::uint64_t level : levels) out->insert(level);
}

// One demand record (R6-a-1 section 5, R6-a-2a 1.7): Info when the set is had and mounted; Error when a needed object could not be had
// (Warning when the caller asked to stop, reason cancelled).
void SayDemand(Env& env, const DemandResult& r, std::uint64_t level, const std::string& gameType) {
  const std::string levelText = FormatHex64(level);
  if (r.ok) {
    env.Event(EventLevel::kInfo, "cdn_demand",
              {{"result", "ok"},
               {"role", RoleName(Role::kServer)},
               {"level", levelText},
               {"game_type", gameType},
               {"needed", r.needed},
               {"cache", r.from_cache},
               {"installed", r.from_installed},
               {"fetched", r.fetched},
               {"bytes", r.bytes},
               {"ms", r.ms},
               {"consequence", r.needed == 0 ? "this map and mode need nothing from the manifest"
                                             : "this session's map and mode run on the manifest's set"}});
    return;
  }
  std::vector<Field> fields = {{"result", "failed"},
                               {"role", RoleName(Role::kServer)},
                               {"level", levelText},
                               {"game_type", gameType},
                               {"reason", r.reason}};
  const bool cancelled = std::string(r.reason) == kCancelled;
  if (!r.label.empty()) {
    fields.push_back({"label", r.label});
    fields.push_back({"name", r.name});
    fields.push_back({"sha256", r.sha256});
  }
  fields.push_back({"needed", r.needed});
  fields.push_back({"fetched", r.fetched});
  fields.push_back({"ms", r.ms});
  fields.push_back({"consequence", "no set is mounted: this session does not start"});
  env.Event(cancelled ? EventLevel::kWarn : EventLevel::kError, "cdn_demand", fields);
}

}  // namespace

bool ParseArchiveBody(const std::vector<std::uint8_t>& body, ArchiveBody* out) {
  *out = ArchiveBody();
  std::size_t at = kFirstByte;
  // A count of records: its bytes must lie inside the body before any record is read.
  const auto count = [&body, &at](std::size_t recordBytes, std::uint64_t* n) {
    if (body.size() - at < kU32Bytes) return false;
    *n = ReadLE(body, at, kU32Bytes);
    at += kU32Bytes;
    return *n <= (body.size() - at) / recordBytes;
  };
  std::uint64_t d = 0;
  if (!count(kU64Bytes, &d)) return false;
  for (std::uint64_t i = 0; i < d; ++i) {
    out->dependents.push_back(ReadLE(body, at, kU64Bytes));
    at += kU64Bytes;
  }
  std::uint64_t n = 0;
  if (!count(kAssetRecordBytes, &n)) return false;
  for (std::uint64_t i = 0; i < n; ++i) {
    out->assets.emplace_back(ReadLE(body, at, kU64Bytes), ReadLE(body, at + kU64Bytes, kU64Bytes));
    at += kAssetRecordBytes;
  }
  // M records are spans the demand does not read; the count must still hold its bytes, and the final u32 must be 0.
  std::uint64_t m = 0;
  if (!count(kBlobRecordBytes, &m)) return false;
  at += static_cast<std::size_t>(m) * kBlobRecordBytes;
  if (body.size() - at != kU32Bytes || ReadLE(body, at, kU32Bytes) != 0) return false;
  return true;
}

std::vector<std::string> LabelsOfMode(std::string_view game_type) {
  std::vector<std::string> out;
  for (const ModeLabels& row : kModeLabels) {
    if (game_type == row.game_type) out.push_back(row.label);
  }
  return out;
}

std::vector<std::uint64_t> LevelsOfMode(std::string_view game_type) {
  std::set<std::uint64_t> out;
  for (const ModeRow& row : kModeRows) {
    if (game_type != row.game_type) continue;
    switch (row.kind) {
      case ModeKind::kArena:
        AddAll(nevr_combat_level::kEchoArenaLevels, &out);
        AddAll(nevr_combat_level::kLobbyLevels, &out);
        break;
      case ModeKind::kCombat:
        AddAll(nevr_combat_level::kCombatMaps, &out);
        out.insert(nevr_combat_level::kCombatLobby);
        AddAll(nevr_combat_level::kLobbyLevels, &out);
        break;
      case ModeKind::kSocial:
        AddAll(nevr_combat_level::kLobbyLevels, &out);
        break;
      case ModeKind::kSocialNpe:
        AddAll(nevr_combat_level::kLobbyLevels, &out);
        out.insert(nevr_combat_level::kTutorialLobbyLevel);
        break;
    }
  }
  return std::vector<std::uint64_t>(out.begin(), out.end());
}

std::vector<std::uint64_t> RootLevels(const DemandInput& input) {
  std::set<std::uint64_t> roots;
  if (input.level != 0) roots.insert(input.level);
  for (const std::uint64_t level : LevelsOfMode(input.game_type)) roots.insert(level);
  return std::vector<std::uint64_t>(roots.begin(), roots.end());
}

bool ComputeNeeded(const Manifest& manifest, const Platform& platform, const Policy& policy, const DemandInput& input,
                   const ReadArchiveFn& read_archive, Needed* out, const char** reason, const Entry** failed) {
  *out = Needed();
  if (failed != nullptr) *failed = nullptr;
  const std::vector<std::uint64_t> roots = RootLevels(input);
  if (roots.empty()) {
    *reason = kNoMode;
    return false;
  }
  // The entries of the labels the policy serves, of this platform, by their key.
  std::map<Key, std::vector<const Entry*>> index;
  for (const Entry& e : manifest.entries) {
    if (e.platform == platform.name && StateOf(e.label, policy) != LabelState::kOff) index[{e.type, e.name}].push_back(&e);
  }
  std::set<const Entry*> taken;
  std::set<std::string> labels;
  const auto take = [&](const std::vector<const Entry*>& found) {
    for (const Entry* e : found) {
      if (!taken.insert(e).second) continue;
      out->entries.push_back(*e);
      labels.insert(e->label);
    }
  };
  // Archive closure: each archive key is visited once. A key the manifest has no entry for is not expanded.
  std::set<Key> visited;
  std::vector<Key> pending;
  for (const std::uint64_t root : roots) pending.push_back({kArchiveType, root});
  while (!pending.empty()) {
    const Key key = pending.back();
    pending.pop_back();
    if (!visited.insert(key).second) continue;
    const auto found = index.find(key);
    if (found == index.end()) continue;
    take(found->second);
    if (key.first != kArchiveType) continue;
    const Entry* archive = found->second.front();
    std::vector<std::uint8_t> body;
    if (!read_archive(*archive, &body)) {
      *reason = kArchiveUnavailable;
      if (failed != nullptr) *failed = archive;
      return false;
    }
    ArchiveBody parsed;
    if (!ParseArchiveBody(body, &parsed)) {
      *reason = kBadArchive;
      if (failed != nullptr) *failed = archive;
      return false;
    }
    for (const std::uint64_t dependent : parsed.dependents) pending.push_back({kArchiveType, dependent});
    for (const auto& asset : parsed.assets) pending.push_back(asset);
  }
  // Mode labels: every entry, record of 0 bytes and script of each label the game type takes whole.
  for (const std::string& label : LabelsOfMode(std::string_view(input.game_type))) {
    if (StateOf(label, policy) == LabelState::kOff) continue;
    for (const Entry& e : manifest.entries) {
      if (e.platform == platform.name && e.label == label) {
        std::vector<const Entry*> one = {&e};
        take(one);
      }
    }
    for (const EmptyRecord& z : manifest.empties) {
      if ((z.platform.empty() || z.platform == platform.name) && z.label == label) out->empties.push_back(z);
    }
    for (const ScriptEntry& s : manifest.scripts) {
      if (s.platform == platform.name && s.label == label) out->scripts.push_back(s);
    }
  }
  out->labels.assign(labels.begin(), labels.end());
  return true;
}

Manifest NeededManifest(const Needed& needed) {
  Manifest out;
  out.labels = needed.labels;
  out.entries = needed.entries;
  out.empties = needed.empties;
  out.scripts = needed.scripts;
  return out;
}

DemandResult ResolveDemand(Env& env, const RunConfig& base, const DemandInput& input, InstalledSource* installed, const Publish& publish,
                           const std::function<bool()>& keep_going) {
  RunConfig config = base;
  config.policy.role = Role::kServer;
  DemandResult r;
  const std::uint64_t started = env.NowMs();
  const auto finish = [&](DemandResult&& res) {
    res.ms = env.NowMs() - started;
    SayDemand(env, res, input.level, input.game_type);
    return res;
  };
  if (RootLevels(input).empty()) {
    r.reason = kNoMode;
    return finish(std::move(r));
  }
  // The deadline ends the resolve at the next boundary; any other stop is the caller's (shutdown).
  const auto going = [&env, &keep_going, started] { return keep_going() && env.NowMs() - started < kServerDemandMs; };
  const auto reasonOf = [&env, started](const char* reason) -> const char* {
    return std::string(reason) == kCancelled && env.NowMs() - started >= kServerDemandMs ? kDeadline : reason;
  };

  if (!env.MakeDirs(config.dir)) {
    r.reason = kCacheDir;
    return finish(std::move(r));
  }
  const Limits limits = detail::LoadLimits(env, config.dir);
  if (!limits.ok) {
    r.reason = kLimitsRefused;
    return finish(std::move(r));
  }
  // Step 1: the manifest, fetched and not saved (a demand resolve never replaces the saved copy).
  detail::ManifestLoad loaded = detail::FetchManifest(env, config, limits, going);
  if (!loaded.ok) {
    r.reason = reasonOf(loaded.stopped);
    return finish(std::move(r));
  }
  const Manifest& manifest = loaded.manifest;
  std::unique_ptr<Store> store = detail::OpenStoreLogged(env, config.dir, limits, "demand");

  // Where each archive was had from, and the archive that failed, for the closure's read.
  std::map<std::string, Place> placed;
  std::uint64_t used = 0;  // stored bytes this resolve has taken from the budget
  const char* archiveReason = "";
  const Entry* archiveFailed = nullptr;
  const ReadArchiveFn readArchive = [&](const Entry& a, std::vector<std::uint8_t>* body) {
    if (a.bytes > limits.max_object_bytes) {
      archiveReason = kBadArchive;
      archiveFailed = &a;
      return false;
    }
    std::string frame;
    std::string text;
    if (store->HoldsSized(a.sha256, a.size, a.bytes)) {  // place 1
      placed[a.sha256] = Place::kCache;
    } else if (installed != nullptr && installed->ReadBody(a, body)) {  // place 2
      placed[a.sha256] = Place::kInstalled;
      return true;
    } else {  // place 3: one archive at a time, as the background fetch does
      if (used + a.size > limits.storage_budget) {
        archiveReason = kOverStorage;
        archiveFailed = &a;
        return false;
      }
      const detail::Downloaded d = detail::Download(env, config, *store, a, going);
      if (!d.ok) {
        archiveReason = d.cancelled ? kCancelled : d.reason;
        archiveFailed = &a;
        return false;
      }
      used += a.size;
      placed[a.sha256] = Place::kCdn;
    }
    store->EndWrites();  // the read opens the store file the write appended to (on the PC, a sharing violation otherwise)
    if (!store->ReadFrame(a.sha256, &frame) || !DecodeFrame(frame, &text)) {
      archiveReason = kBadArchive;
      archiveFailed = &a;
      return false;
    }
    *body = BytesOf(text);
    return true;
  };

  Needed needed;
  const char* reason = "";
  const Entry* failed = nullptr;
  if (!ComputeNeeded(manifest, config.platform, config.policy, input, readArchive, &needed, &reason, &failed)) {
    if (archiveFailed == nullptr) archiveFailed = failed;
    r.reason = archiveReason[0] != '\0' ? reasonOf(archiveReason) : reason;
    if (archiveFailed != nullptr) {
      r.label = archiveFailed->label;
      r.name = FormatHex64(archiveFailed->name);
      r.sha256 = archiveFailed->sha256;
    }
    return finish(std::move(r));
  }
  r.needed = needed.entries.size();

  // Places 1 and 2 for the assets; the rest is the CDN's, all or nothing.
  std::vector<Entry> fetch;
  std::set<std::string> counted;
  for (const Entry& e : needed.entries) {
    if (!counted.insert(e.sha256).second) continue;
    const auto archivePlace = placed.find(e.sha256);
    if (archivePlace != placed.end()) {
      if (archivePlace->second == Place::kCache) ++r.from_cache;
      if (archivePlace->second == Place::kInstalled) ++r.from_installed;
      if (archivePlace->second == Place::kCdn) ++r.fetched;
      if (archivePlace->second == Place::kCdn) r.bytes += e.size;
      continue;
    }
    if (store->HoldsSized(e.sha256, e.size, e.bytes)) {
      ++r.from_cache;
    } else if (installed != nullptr && installed->ReadVerified(e)) {
      ++r.from_installed;
      placed[e.sha256] = Place::kInstalled;
    } else {
      fetch.push_back(e);
    }
  }
  // Every archive counted above was had through the closure, so `fetch` holds only assets that no place had.
  std::uint64_t freshBytes = 0;
  for (const Entry& e : fetch) freshBytes += e.size;
  if (used + freshBytes > limits.storage_budget) {
    r.reason = kOverStorage;
    return finish(std::move(r));
  }
  if (!fetch.empty()) {
    PrefetchConfig pc;
    pc.base_url = config.base_url;
    pc.objects = fetch;
    pc.keep_going = going;
    detail::StorePrefetch adapter(*store, pc.objects);
    const PrefetchResult pr = Prefetch(env, adapter, pc);
    r.fetched += pr.fetched;
    r.bytes += pr.bytes_fetched;
    if (pr.status != PrefetchStatus::kComplete) {
      r.reason = pr.status == PrefetchStatus::kCancelled ? reasonOf(kCancelled) : PrefetchStatusName(pr.status);
      for (const Entry& e : fetch) {
        if (e.sha256 == pr.failed_sha256) {
          r.label = e.label;
          r.name = FormatHex64(e.name);
          r.sha256 = e.sha256;
        }
      }
      return finish(std::move(r));
    }
  }
  // Complete: mount what is needed, minus the objects the installed files hold, and publish it once.
  Needed mounted = needed;
  mounted.entries.erase(std::remove_if(mounted.entries.begin(), mounted.entries.end(),
                                       [&placed](const Entry& e) {
                                         const auto p = placed.find(e.sha256);
                                         return p != placed.end() && p->second == Place::kInstalled;
                                       }),
                        mounted.entries.end());
  if (!mounted.entries.empty()) {
    store->EndWrites();
    const Manifest mountSet = NeededManifest(mounted);
    std::unique_ptr<Mounted> set(new Mounted(Mount(env, *store, mountSet, config.platform, detail::GatedPolicy(env, config, mountSet, "demand"))));
    if (set->shadow) {
      publish(std::move(set));
      r.published = true;
    }
  }
  r.ok = true;
  return finish(std::move(r));
}

}  // namespace nevr_cdn
