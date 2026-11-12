#include "core/map_run.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "core/map_index.h"
#include "nevr/symbol_hash.h"

namespace nevr_maps {
namespace {

using nevr_cargo::Env;
using nevr_cargo::EventLevel;
using nevr_cargo::Field;
using nevr_cargo::HttpResult;

constexpr const char* kBlobPrefix = "blobs/sha256/";
constexpr const char* kManifestUrlPath = "v1/maps/manifest.signed";
constexpr const char* kPackageFile = "package.json";
constexpr const char* kPiecesDir = "pieces";
constexpr std::size_t kStreamBytes = std::size_t{1} << 20;
constexpr std::size_t kMaxSignedBytes = kMaxManifestBytes + kMaxMinisigBytes + 16;  // minisig, "--" line, body
constexpr int kDownloadAttempts = 4;                                                // the first try and 3 retries
constexpr std::uint32_t kMaxChunkBytes = 128u << 20;                                // the registry's kMaxResourceBytes
constexpr std::size_t kFrameHeaderMax = 18;

std::string Join(const std::string& a, const std::string& b) { return a + "/" + b; }

std::string Owner(const std::string& id) { return "map:" + id; }

std::string ParentOf(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string HttpReason(long status) { return status == 0 ? "transport" : "http_" + std::to_string(status); }

bool Contains(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

std::vector<std::string> SortedUnique(const std::vector<std::string>& ids) {
  std::set<std::string> unique(ids.begin(), ids.end());
  return std::vector<std::string>(unique.begin(), unique.end());
}

std::string ExpectedChannel(const ChannelConfig& config, const FloorBook& floors) {
  if (!config.channel.empty()) return config.channel;
  const auto bound = floors.bound.find(config.base_url);
  return bound != floors.bound.end() ? bound->second : std::string();
}

bool ServerAllows(const ChannelConfig& config, const std::string& id, const std::string& index_sha256) {
  return Contains(config.server_allow, id) || Contains(config.server_allow, id + "@" + index_sha256);
}

// An empty list on either side is not checked: the package names no stock manifest, or the runtime knows none.
bool StockManifestOk(const Package& package, const ChannelConfig& config) {
  if (package.requires_stock_manifest_sha256.empty() || config.stock_manifest_sha256.empty()) return true;
  for (const std::string& sha : package.requires_stock_manifest_sha256) {
    if (Contains(config.stock_manifest_sha256, sha)) return true;
  }
  return false;
}

bool GameBuildOk(const ChannelConfig& config, const std::string& game_build) {
  return config.game_build.empty() || game_build.empty() || config.game_build == game_build;
}

bool HasScripts(const Package& package) {
  if (!package.rekeyed.empty()) return true;
  for (const Member& m : package.members) {
    if (m.role == Role::kScript) return true;
  }
  return false;
}

bool NeedsDerive(const Package& package) {
  if (!package.derived.empty()) return true;
  for (const RekeyPin& r : package.rekeyed) {
    if (!r.edits.empty()) return true;
  }
  return false;
}

// The first revoked sha256 among the package index, its members and their pieces; empty when none.
std::string RevokedSha(const Manifest& manifest, const std::string& index_sha256, const Package& package) {
  if (manifest.revoked_sha256.empty()) return std::string();
  const std::set<std::string> revoked(manifest.revoked_sha256.begin(), manifest.revoked_sha256.end());
  if (revoked.count(index_sha256)) return index_sha256;
  for (const Member& m : package.members) {
    if (revoked.count(m.sha256)) return m.sha256;
    for (const Pin& p : m.pieces) {
      if (revoked.count(p.sha256)) return p.sha256;
    }
  }
  return std::string();
}

// The manifest entry a cached version was fetched under, rebuilt from the index so an older cached version parses too.
PlatformEntry EntryOf(const CachedMap& cached) {
  PlatformEntry entry;
  entry.version = cached.version;
  entry.index.sha256 = cached.index_sha256;
  entry.index.size = cached.index_size;
  entry.level = cached.level;
  entry.level_name = cached.level_name;
  entry.game_build = cached.game_build;
  return entry;
}

std::string JoinPairs(const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::string text;
  for (const auto& [id, token] : pairs) {
    if (!text.empty()) text += ',';
    text += id + ":" + token;
  }
  return text;
}

std::vector<StockStamp> UniqueByPath(const std::vector<StockStamp>& stamps) {
  std::vector<StockStamp> out;
  std::set<std::string> seen;
  for (const StockStamp& s : stamps) {
    if (seen.insert(s.path).second) out.push_back(s);
  }
  return out;
}

std::uint32_t LoadU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint64_t LoadU64(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(LoadU32(p)) | static_cast<std::uint64_t>(LoadU32(p + 4)) << 32;
}

bool Inside(std::uint64_t offset, std::uint64_t length, std::uint64_t total) {
  return offset <= total && length <= total - offset;
}

// The content size a zstd frame header states, as ZSTD_getFrameContentSize reads it; false when it states none or the
// header is malformed or cut short.
bool FrameContentSize(const std::uint8_t* header, std::size_t available, std::uint64_t* out) {
  if (available < 5 || LoadU32(header) != 0xFD2FB528u) return false;
  const std::uint8_t descriptor = header[4];
  if (descriptor & 0x08) return false;  // reserved bit
  const unsigned size_flag = descriptor >> 6;
  const bool single_segment = (descriptor & 0x20) != 0;
  static constexpr std::size_t kDictIdBytes[4] = {0, 1, 2, 4};
  std::size_t at = 5 + (single_segment ? 0 : 1) + kDictIdBytes[descriptor & 3];
  const std::size_t size_bytes = size_flag == 0 ? (single_segment ? 1 : 0) : (std::size_t{1} << size_flag);
  if (size_bytes == 0 || at + size_bytes > available) return false;
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size_bytes; ++i) value |= static_cast<std::uint64_t>(header[at + i]) << (8 * i);
  if (size_bytes == 2) value += 256;
  *out = value;
  return true;
}

bool Fail(std::string* problem, const std::string& why) {
  *problem = why;
  return false;
}

bool ReadExact(Env& env, std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length) {
  std::size_t got = 0;
  return env.ReadBlob(handle, offset, buffer, length, &got) && got == length;
}

bool WalkOpenSet(Env& env, std::int64_t handle, std::uint64_t size, std::string* problem) {
  std::uint8_t header[48];
  if (size < sizeof header || !ReadExact(env, handle, 0, header, sizeof header)) return Fail(problem, "too_small");
  if (std::memcmp(header, "NEVRRES1", 8) != 0) return Fail(problem, "magic");
  if (LoadU32(header + 0x08) != sizeof header) return Fail(problem, "header");
  const std::uint32_t records = LoadU32(header + 0x0c), chunks = LoadU32(header + 0x10),
                      overrides = LoadU32(header + 0x14);
  const std::uint64_t record_table = LoadU64(header + 0x18), chunk_table = LoadU64(header + 0x20),
                      override_table = LoadU64(header + 0x28);
  if (records == 0) return Fail(problem, "no_records");
  if (!Inside(record_table, std::uint64_t{records} * 0x20, size) ||
      !Inside(chunk_table, std::uint64_t{chunks} * 0x10, size) ||
      !Inside(override_table, std::uint64_t{overrides} * 0x18, size)) {
    return Fail(problem, "table_range");
  }
  constexpr std::uint32_t kBatch = static_cast<std::uint32_t>(kStreamBytes / 0x10);
  std::vector<std::uint8_t> table(std::size_t{std::min(chunks, kBatch)} * 0x10);
  for (std::uint32_t first = 0; first < chunks; first += kBatch) {
    const std::uint32_t count = std::min(kBatch, chunks - first);
    if (!ReadExact(env, handle, chunk_table + std::uint64_t{first} * 0x10, table.data(), std::size_t{count} * 0x10)) {
      return Fail(problem, "read");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint8_t* c = table.data() + std::size_t{i} * 0x10;
      const std::uint32_t reserved = LoadU32(c), offset = LoadU32(c + 4), compressed = LoadU32(c + 8),
                          decompressed = LoadU32(c + 12);
      const std::string where = " chunk " + std::to_string(first + i);
      if (reserved != 0) return Fail(problem, "chunk_reserved" + where);
      if (compressed == 0 || !Inside(offset, compressed, size)) return Fail(problem, "chunk_range" + where);
      if (decompressed == 0 || decompressed > kMaxChunkBytes) return Fail(problem, "chunk_size" + where);
      std::uint8_t frame[kFrameHeaderMax];
      const std::size_t length = std::min<std::size_t>(kFrameHeaderMax, compressed);
      if (!ReadExact(env, handle, offset, frame, length)) return Fail(problem, "read");
      if (LoadU32(frame) != 0xFD2FB528u) return Fail(problem, "chunk_not_zstd" + where);
      std::uint64_t stated = 0;
      if (!FrameContentSize(frame, length, &stated) || stated != decompressed)
        return Fail(problem, "frame_size" + where);
    }
  }
  return true;
}

// Every file one cached map mounts, as the boot opens them.
struct Opened {
  const CachedFile* file = nullptr;
  std::int64_t handle = 0;
  bool granted = false;  // the mounter owns the handle
};

class BootRun {
 public:
  BootRun(Env& env, MapFs& fs, const ChannelConfig& config, Mounter& mounter, Deriver* deriver)
      : env_(env), fs_(fs), config_(config), mounter_(mounter), deriver_(deriver) {}

  BootReport Run(const std::string& now_utc) {
    const std::uint64_t started = env_.NowMs();
    const std::vector<std::string> ids = SortedUnique(config_.subscribed);
    std::string failure = Load(now_utc);
    if (!failure.empty()) {
      for (const std::string& id : ids) Skip(id, failure, /*log=*/false);
      Summary();
      return std::move(report_);
    }
    for (const auto& [id, entry] : parsed_.manifest.maps) {
      if (entry.platforms.count(config_.platform) && !Contains(parsed_.manifest.revoked_ids, id)) {
        report_.listed.emplace_back(EchoVR::CSymbol64Hash(id.c_str()), id);
      }
    }
    for (const std::string& id : ids) {
      if (config_.boot_budget_ms != 0 && env_.NowMs() - started >= config_.boot_budget_ms) {
        Skip(id, "boot_budget");
        continue;
      }
      MountOne(id);
    }
    Summary();
    return std::move(report_);
  }

 private:
  // The cache index and the stored manifest, verified again against the floors and `now`. Returns a reason or "".
  std::string Load(const std::string& now_utc) {
    const CacheIndexStatus status = ReadCacheIndex(env_, config_.dir, &index_);
    if (status != CacheIndexStatus::kOk) {
      env_.Event(status == CacheIndexStatus::kAbsent ? EventLevel::kInfo : EventLevel::kWarn, "map_boot",
                 {{"reason", "no_index"}, {"index", CacheIndexStatusName(status)}});
      return "no_index";
    }
    std::string signed_text;
    if (!env_.ReadSmallFile(Join(config_.dir, kStoredManifestFile), &signed_text, kMaxSignedBytes)) {
      env_.Event(EventLevel::kWarn, "map_boot", {{"reason", "no_manifest"}});
      return "no_manifest";
    }
    parsed_ = ParseAndVerifyManifest(signed_text, config_.keys, index_.floors, ExpectedChannel(config_, index_.floors),
                                     now_utc);
    if (!parsed_.reason.empty()) {
      env_.Event(EventLevel::kWarn, "map_boot", {{"reason", parsed_.reason}});
      return parsed_.reason;
    }
    expired_ = IsExpired(parsed_.manifest.expires, now_utc);
    env_.Event(EventLevel::kInfo, "map_boot",
               {{"serial", parsed_.manifest.serial},
                {"channel", parsed_.channel},
                {"expired", expired_},
                {"cached", index_.maps.size()}});
    return std::string();
  }

  void NotMounted(std::vector<std::string>* list, const std::string& id, const std::string& token) {
    list->push_back(id);
    report_.not_mounted.emplace_back(id, token);
  }

  void Skip(const std::string& id, const std::string& token, bool log = true) {
    NotMounted(&report_.skipped, id, token);
    if (log) env_.Event(EventLevel::kWarn, "map_skipped", {{"id", id}, {"reason", token}});
  }

  void Defer(const std::string& id, const std::string& token) {
    NotMounted(&report_.deferred, id, token);
    env_.Event(EventLevel::kWarn, "map_deferred", {{"id", id}, {"reason", token}});
  }

  void Refuse(const std::string& id, const std::string& token, const std::string& holder = std::string()) {
    report_.refused.emplace_back(id, token);
    report_.not_mounted.emplace_back(id, token);
    std::vector<Field> fields{{"id", id}, {"refused", token}};
    if (!holder.empty()) fields.emplace_back("holder", Owner(holder));
    env_.Event(EventLevel::kWarn, "map_mounted", fields);
  }

  // Once per distinct (path, stamp) per boot, however many maps' recipes read that stock file.
  bool StockCurrent(const std::vector<StockStamp>& stamps) {
    for (const StockStamp& s : stamps) {
      const std::string key = s.path + '\n' + std::to_string(s.stamp.volume) + ':' + std::to_string(s.stamp.id_hi) +
                              ':' + std::to_string(s.stamp.id_lo) + ':' + std::to_string(s.stamp.size) + ':' +
                              std::to_string(s.stamp.last_write);
      auto known = stock_checked_.find(key);
      if (known == stock_checked_.end()) known = stock_checked_.emplace(key, deriver_->StockCurrent({s})).first;
      if (!known->second) return false;
    }
    return true;
  }

  void CloseUngranted(const std::vector<Opened>& opened) {
    for (const Opened& o : opened) {
      if (!o.granted) env_.CloseBlob(o.handle);
    }
  }

  void MountOne(const std::string& id) {
    if (Contains(config_.local_maps, id)) return Skip(id, "local_map");
    if (Contains(parsed_.manifest.revoked_ids, id)) return Refuse(id, "revoked");
    const auto listed = parsed_.manifest.maps.find(id);
    if (listed == parsed_.manifest.maps.end() || !listed->second.platforms.count(config_.platform)) {
      return Skip(id, "not_listed");
    }
    if (Contains(parsed_.manifest.revoked_sha256, listed->second.platforms.at(config_.platform).index.sha256)) {
      return Refuse(id, "revoked");
    }
    const auto found = index_.maps.find(MapKey(id, config_.platform));
    if (found == index_.maps.end() || found->second.pinned_serial == 0) return Skip(id, "not_cached");
    const CachedMap& cached = found->second;
    const std::string version_dir = Join(config_.dir, cached.dir);

    // ParsePackageIndex checks the bytes against the pin the index recorded: index_pin means the file changed on disk.
    std::string package_bytes;
    if (!env_.ReadSmallFile(Join(version_dir, kPackageFile), &package_bytes, kMaxIndexBytes))
      return Defer(id, "missing");
    const ParsedPackage parsed = ParsePackageIndex(package_bytes, id, config_.platform, EntryOf(cached));
    if (parsed.reason == "index_pin") return Defer(id, "package_hash");
    if (!parsed.reason.empty()) return Refuse(id, parsed.reason);
    const Package& package = parsed.package;

    if (!RevokedSha(parsed_.manifest, cached.index_sha256, package).empty()) return Refuse(id, "revoked");
    if (!GameBuildOk(config_, cached.game_build)) return Refuse(id, "unsupported_game");
    if (!StockManifestOk(package, config_)) return Refuse(id, "stock_mismatch");
    const bool scripts = HasScripts(package);
    if (scripts && expired_) return Refuse(id, "expired");
    if (scripts && config_.signed_only) return Refuse(id, "signed_only");
    if (scripts && config_.is_server && !ServerAllows(config_, id, cached.index_sha256))
      return Refuse(id, "server_not_allowed");
    const bool derive = NeedsDerive(package);
    if ((derive || !cached.stock_stamps.empty()) && deriver_ == nullptr) return Refuse(id, "derived_unsupported");
    if (!cached.stock_stamps.empty() && !StockCurrent(cached.stock_stamps)) return Defer(id, "stock_changed");

    // The files this map mounts: the set (the merged one when recipes rebuilt it), the published scripts, the rebuilt
    // ones.
    const CachedFile* set = nullptr;
    std::map<std::uint64_t, const CachedFile*> published, rebuilt;
    const FileRole set_role = package.derived.empty() ? FileRole::kSet : FileRole::kMergedSet;
    for (const CachedFile& f : cached.files) {
      if (f.role == set_role) set = &f;
      if (f.role == FileRole::kScript) published[f.script_hash] = &f;
      if (f.role == FileRole::kDerivedScript) rebuilt[f.script_hash] = &f;
    }
    if (set == nullptr) return Defer(id, "missing");
    std::vector<const RekeyPin*> rekeys, derived_scripts;
    for (const Member& m : package.members) {
      if (m.role == Role::kScript && !published.count(m.script_hash)) return Defer(id, "missing");
    }
    for (const RekeyPin& r : package.rekeyed) {
      if (r.edits.empty()) {
        rekeys.push_back(&r);
      } else if (!rebuilt.count(r.hash)) {
        return Defer(id, "missing");
      } else {
        derived_scripts.push_back(&r);
      }
    }

    // Open and hold every file first, so a stamp that moved defers the map before anything is granted.
    std::vector<Opened> opened;
    std::vector<const CachedFile*> wanted{set};
    for (const auto& [hash, file] : published) wanted.push_back(file);
    for (const RekeyPin* r : derived_scripts) wanted.push_back(rebuilt[r->hash]);
    for (const CachedFile* file : wanted) {
      Opened o;
      o.file = file;
      std::uint64_t size = 0;
      if (!fs_.OpenHeld(Join(version_dir, file->path), &o.handle, &size)) {
        CloseUngranted(opened);
        return Defer(id, "missing");
      }
      opened.push_back(o);
      FileStamp stamp;
      if (!fs_.StampOf(o.handle, &stamp)) {
        CloseUngranted(opened);
        return Defer(id, "stamp_unsupported");
      }
      if (stamp != file->stamp) {
        CloseUngranted(opened);
        return Defer(id, "stamp");
      }
    }

    const std::string owner = Owner(id);
    std::string holder;
    auto refuse_scripts = [&](const std::string& reason) {
      mounter_.Withdraw(owner);
      CloseUngranted(opened);
      Refuse(id, "script:" + reason, holder);
    };
    auto held_by_other = [&](std::uint64_t hash) {
      const auto it = script_holders_.find(hash);
      if (it == script_holders_.end() || it->second == id) return false;
      holder = it->second;
      return true;
    };
    for (std::size_t i = 1; i < opened.size(); ++i) {
      Opened& o = opened[i];
      const bool is_rebuilt = o.file->role == FileRole::kDerivedScript;
      if (held_by_other(o.file->script_hash)) return refuse_scripts("duplicate");
      Grant grant;
      if (is_rebuilt) {
        const auto pin = std::find_if(derived_scripts.begin(), derived_scripts.end(),
                                      [&](const RekeyPin* r) { return r->hash == o.file->script_hash; });
        grant = mounter_.DerivedScript(owner, o.file->script_hash, o.handle, o.file->sha256, (*pin)->stock_sha256);
      } else {
        grant = mounter_.Script(owner, o.file->script_hash, o.handle, o.file->sha256);
      }
      if (!grant.ok) return refuse_scripts(grant.reason);
      o.granted = true;
    }
    for (const RekeyPin* r : rekeys) {
      if (held_by_other(r->hash)) return refuse_scripts("duplicate");
      const Grant grant = mounter_.Rekey(owner, r->hash, r->stock_source, r->stock_sha256);
      if (!grant.ok) return refuse_scripts(grant.reason);
    }

    const Grant set_grant =
        mounter_.Set(owner, opened[0].handle, owner + "@" + cached.version, set->sha256, set->frames_checked);
    if (!set_grant.ok) {
      mounter_.Withdraw(owner);
      CloseUngranted(opened);
      return Refuse(id, set_grant.reason);
    }
    opened[0].granted = true;
    const std::string alias_refusal =
        StageAliasesOrWithdraw(mounter_, owner, id, package.level, package.config_aliases);
    if (!alias_refusal.empty()) return Refuse(id, alias_refusal);
    for (std::size_t i = 1; i < opened.size(); ++i) script_holders_[opened[i].file->script_hash] = id;
    for (const RekeyPin* r : rekeys) script_holders_[r->hash] = id;
    report_.mounted.push_back(id);
    MountedMap staged;
    staged.id = id;
    staged.version = cached.version;
    staged.index_sha256 = cached.index_sha256;
    staged.platform = config_.platform;
    staged.source_sha256 = package.source_sha256;
    staged.title = listed->second.title;
    staged.level = package.level;
    report_.staged.push_back(std::move(staged));
    env_.Event(EventLevel::kInfo, "map_mounted",
               {{"id", id},
                {"version", cached.version},
                {"scripts", opened.size() - 1 + rekeys.size()},
                {"verified_stamp", set->frames_checked}});
  }

  void Summary() {
    const std::vector<std::pair<std::string, std::string>>& refused = report_.refused;
    std::vector<std::pair<std::string, std::string>> skipped, deferred;
    for (const auto& [id, token] : report_.not_mounted) {
      if (Contains(report_.skipped, id)) skipped.emplace_back(id, token);
      if (Contains(report_.deferred, id)) deferred.emplace_back(id, token);
    }
    const bool warn = !refused.empty() || !skipped.empty() || !deferred.empty();
    env_.Event(warn ? EventLevel::kWarn : EventLevel::kInfo, "map_summary",
               {{"mounted", report_.mounted.size()},
                {"refused", JoinPairs(refused)},
                {"skipped", JoinPairs(skipped)},
                {"deferred", JoinPairs(deferred)}});
  }

  Env& env_;
  MapFs& fs_;
  const ChannelConfig& config_;
  Mounter& mounter_;
  Deriver* deriver_;
  CacheIndex index_;
  ParsedManifest parsed_;
  bool expired_ = false;
  BootReport report_;
  std::map<std::uint64_t, std::string> script_holders_;  // script or rekey hash -> the id that mounted it
  std::map<std::string, bool> stock_checked_;
};

struct PieceLocation {
  std::size_t file = 0;
  std::uint64_t offset = 0, size = 0;
};

struct Download {
  std::string reason;  // "" on success
  bool retry = false;
};

class FetchRun {
 public:
  FetchRun(Env& env, MapFs& fs, const ChannelConfig& config, Deriver* deriver, const std::function<bool()>& keep_going)
      : env_(env), fs_(fs), config_(config), deriver_(deriver), keep_going_(keep_going), buffer_(kStreamBytes) {}

  FetchReport Run(const std::vector<std::string>& deferred, const std::string& now_utc) {
    if (!env_.MakeDirs(config_.dir)) return Finish("cache_dir");
    const CacheIndexStatus status = ReadCacheIndex(env_, config_.dir, &index_);
    if (status != CacheIndexStatus::kOk && status != CacheIndexStatus::kAbsent) {
      env_.Event(EventLevel::kWarn, "map_index",
                 {{"reason", CacheIndexStatusName(status)}, {"consequence", "the cache is treated as empty"}});
      dirty_ = true;
    }
    subscribed_ = SortedUnique(config_.subscribed);
    CollectGarbage();
    BuildCatalog();
    Rehash(SortedUnique(deferred));
    if (!keep_going_()) return Finish("cancelled");
    if (config_.base_url.empty()) return Finish("no_base_url");

    std::string signed_text;
    const std::string failure = FetchManifest(&signed_text);
    if (!failure.empty()) {
      env_.Event(EventLevel::kWarn, "map_manifest",
                 {{"reason", failure}, {"consequence", "the stored manifest is kept"}});
      return Finish(failure);
    }
    parsed_ = ParseAndVerifyManifest(signed_text, config_.keys, index_.floors, ExpectedChannel(config_, index_.floors),
                                     now_utc);
    if (!parsed_.reason.empty()) {
      env_.Event(EventLevel::kWarn, "map_manifest",
                 {{"reason", parsed_.reason}, {"consequence", "the stored manifest is kept"}});
      return Finish(parsed_.reason);
    }
    for (const auto& [key, reason] : parsed_.dropped) {
      env_.Event(EventLevel::kWarn, "map_manifest", {{"dropped", key}, {"reason", reason}});
    }
    env_.Event(
        EventLevel::kInfo, "map_manifest",
        {{"serial", parsed_.manifest.serial}, {"channel", parsed_.channel}, {"maps", parsed_.manifest.maps.size()}});

    PlanAndFetch();

    // The commit order: the manifest, then the floors it raises, then the index naming what is in place, last.
    std::string reason = keep_going_() ? std::string() : "cancelled";
    if (!env_.WriteFileAtomically(Join(config_.dir, kStoredManifestFile), signed_text)) {
      reason = "manifest_write";
    } else {
      index_.floors = AdvanceFloors(index_.floors, config_.base_url, parsed_);
      dirty_ = true;
    }
    return Finish(reason);
  }

 private:
  FetchReport Finish(const std::string& reason) {
    report_.reason = reason;
    if (dirty_ && !WriteCacheIndex(env_, config_.dir, index_) && report_.reason.empty()) report_.reason = "index_write";
    env_.Event(report_.reason.empty() || report_.reason == "cancelled" ? EventLevel::kInfo : EventLevel::kWarn,
               "map_done",
               {{"reason", report_.reason.empty() ? "ok" : report_.reason},
                {"maps_updated", report_.maps_updated},
                {"pieces_fetched", report_.pieces_fetched},
                {"pieces_reused", report_.pieces_reused},
                {"bytes_fetched", report_.bytes_fetched}});
    return report_;
  }

  void Drop(const std::string& key) {
    if (index_.maps.erase(key)) dirty_ = true;
  }

  // Unsubscribed ids, other platforms, *.tmp assemblies and version directories the index does not name are removed; an
  // index entry whose directory is gone is dropped. A directory the boot mount holds open fails to go and is tried
  // again next time.
  void CollectGarbage() {
    std::vector<std::string> names;
    if (!fs_.ListDir(config_.dir, &names)) return;
    std::set<std::string> present;
    for (const std::string& id : names) {
      if (!IsValidMapId(id)) continue;  // index.json, manifest.signed, *.bad, ~*.part, and anything not ours
      const std::string id_dir = Join(config_.dir, id);
      if (!std::binary_search(subscribed_.begin(), subscribed_.end(), id)) {
        Remove(id_dir, id, "unsubscribed");
        continue;
      }
      std::vector<std::string> platforms;
      if (!fs_.ListDir(id_dir, &platforms)) continue;
      for (const std::string& platform : platforms) {
        const std::string platform_dir = Join(id_dir, platform);
        if (platform != config_.platform) {
          Remove(platform_dir, MapKey(id, platform), "platform");
          continue;
        }
        const auto entry = index_.maps.find(MapKey(id, platform));
        std::vector<std::string> versions;
        if (!fs_.ListDir(platform_dir, &versions)) continue;
        for (const std::string& version : versions) {
          const std::string rel = id + "/" + platform + "/" + version;
          if (entry != index_.maps.end() && entry->second.dir == rel) {
            present.insert(entry->first);
          } else {
            Remove(Join(platform_dir, version), rel, "unreferenced");
          }
        }
      }
    }
    for (auto it = index_.maps.begin(); it != index_.maps.end();) {
      if (!present.count(it->first)) {
        env_.Event(EventLevel::kInfo, "map_gc", {{"dropped", it->first}, {"reason", "missing_or_unsubscribed"}});
        it = index_.maps.erase(it);
        dirty_ = true;
      } else {
        ++it;
      }
    }
  }

  void Remove(const std::string& path, const std::string& what, const char* reason) {
    const bool removed = fs_.RemoveTree(path);
    env_.Event(removed ? EventLevel::kInfo : EventLevel::kWarn, "map_gc",
               {{"removed", what}, {"reason", reason}, {"result", removed ? "ok" : "held"}});
  }

  bool ReadPackage(const CachedMap& cached, Package* out) {
    std::string bytes;
    if (!env_.ReadSmallFile(Join(Join(config_.dir, cached.dir), kPackageFile), &bytes, kMaxIndexBytes)) return false;
    ParsedPackage parsed = ParsePackageIndex(bytes, cached.id, cached.platform, EntryOf(cached));
    if (!parsed.reason.empty()) return false;
    *out = std::move(parsed.package);
    return true;
  }

  // Every piece of every published member under `version_dir`: sha256 -> (file, offset, size). A later location
  // replaces an earlier one, so a piece the fetch assembled this pass is found in its final directory.
  void AddToCatalog(const std::string& version_dir, const Package& package) {
    for (const Member& m : package.members) {
      catalog_files_.push_back(Join(version_dir, m.path));
      std::uint64_t offset = 0;
      for (const Pin& p : m.pieces) {
        catalog_[p.sha256] = PieceLocation{catalog_files_.size() - 1, offset, p.size};
        offset += p.size;
      }
    }
  }

  void BuildCatalog() {
    for (const auto& [key, cached] : index_.maps) {
      Package package;
      if (ReadPackage(cached, &package)) AddToCatalog(Join(config_.dir, cached.dir), package);
    }
  }

  bool HashFile(const std::string& path, std::string* sha256, FileStamp* stamp) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!fs_.OpenHeld(path, &handle, &size)) return false;
    std::unique_ptr<nevr_cargo::Hasher> hasher = env_.NewHasher();
    bool ok = fs_.StampOf(handle, stamp);
    for (std::uint64_t at = 0; ok && at < size;) {
      const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(), size - at));
      ok = ReadExact(env_, handle, at, buffer_.data(), length) && keep_going_();
      if (ok) hasher->Update(buffer_.data(), length);
      at += length;
    }
    env_.CloseBlob(handle);
    if (ok) *sha256 = hasher->Hex();
    return ok;
  }

  // The boot deferred these maps: equal bytes refresh the stamps, other bytes drop the entry so the plan fetches it
  // again. A map whose stock files changed is rebuilt here, from its cached members, even though its index is
  // unchanged.
  void Rehash(const std::vector<std::string>& deferred) {
    for (const std::string& id : deferred) {
      if (!keep_going_()) return;
      const std::string key = MapKey(id, config_.platform);
      const auto found = index_.maps.find(key);
      if (found == index_.maps.end()) continue;
      CachedMap& cached = found->second;
      const std::string version_dir = Join(config_.dir, cached.dir);
      bool equal = true;
      for (CachedFile& f : cached.files) {
        std::string sha;
        FileStamp stamp;
        if (!HashFile(Join(version_dir, f.path), &sha, &stamp) || sha != f.sha256) {
          equal = false;
          break;
        }
        f.stamp = stamp;
      }
      if (!keep_going_()) return;
      dirty_ = true;
      if (!equal) {
        env_.Event(EventLevel::kWarn, "map_rehash", {{"id", id}, {"result", "dropped"}, {"reason", "member_hash"}});
        index_.maps.erase(found);
        continue;
      }
      if (!cached.stock_stamps.empty() &&
          (deriver_ == nullptr || !deriver_->StockCurrent(UniqueByPath(cached.stock_stamps)))) {
        Package package;
        std::string reason =
            deriver_ == nullptr ? "derived_unsupported" : (ReadPackage(cached, &package) ? "" : "package");
        if (reason.empty()) reason = Rederive(version_dir, package, &cached);
        if (!reason.empty()) {
          env_.Event(EventLevel::kWarn, "map_rehash", {{"id", id}, {"result", "dropped"}, {"reason", reason}});
          index_.maps.erase(found);
          continue;
        }
        env_.Event(EventLevel::kInfo, "map_rehash", {{"id", id}, {"result", "rebuilt"}});
        continue;
      }
      env_.Event(EventLevel::kInfo, "map_rehash", {{"id", id}, {"result", "refreshed"}});
    }
  }

  // Derive in place over a version directory nothing holds (the boot deferred the map): the rebuilt files and their
  // stamps replace the old ones in `cached`.
  std::string Rederive(const std::string& version_dir, const Package& package, CachedMap* cached) {
    std::vector<CachedFile> files;
    for (const CachedFile& f : cached->files) {
      if (f.role == FileRole::kSet || f.role == FileRole::kScript) files.push_back(f);
    }
    std::vector<StockStamp> stock;
    std::string reason = RunDerive(version_dir, package, &files, &stock);
    if (!reason.empty()) return reason;
    for (CachedFile& f : files) {
      if (f.role == FileRole::kSet || f.role == FileRole::kScript) continue;
      if (!Stamp(Join(version_dir, f.path), &f.stamp)) return "stamp";
    }
    cached->files = std::move(files);
    cached->stock_stamps = std::move(stock);
    return std::string();
  }

  // Runs the deriver over `version_dir` and appends the merged set and the rebuilt scripts to `files`, frames walked.
  // Stamps are left for the caller, which takes them where the files finally live.
  std::string RunDerive(const std::string& version_dir, const Package& package, std::vector<CachedFile>* files,
                        std::vector<StockStamp>* stock_stamps) {
    const DeriveOutput out = deriver_->Derive(package, version_dir, keep_going_);
    if (!keep_going_()) return "cancelled";
    if (!out.reason.empty()) return out.reason;
    if (!package.derived.empty()) {
      CachedFile merged;
      merged.role = FileRole::kMergedSet;
      merged.sha256 = out.set_sha256;
      if (!RelativeTo(version_dir, out.set_path, &merged.path) || out.set_sha256.empty()) return "derived_output";
      std::string problem;
      if (!SizeOf(Join(version_dir, merged.path), &merged.size)) return "derived_output";
      merged.frames_checked = WalkSetFrames(env_, Join(version_dir, merged.path), &problem);
      files->push_back(std::move(merged));
    }
    for (const RekeyPin& r : package.rekeyed) {
      if (r.edits.empty()) continue;
      const auto script =
          std::find_if(out.scripts.begin(), out.scripts.end(),
                       [&](const std::pair<std::uint64_t, std::string>& s) { return s.first == r.hash; });
      CachedFile rebuilt;
      rebuilt.role = FileRole::kDerivedScript;
      rebuilt.script_hash = r.hash;
      rebuilt.sha256 = r.result_sha256;
      if (script == out.scripts.end() || !RelativeTo(version_dir, script->second, &rebuilt.path) ||
          !SizeOf(Join(version_dir, rebuilt.path), &rebuilt.size) || rebuilt.size != r.result_size) {
        return "derived_output";
      }
      files->push_back(std::move(rebuilt));
    }
    *stock_stamps = UniqueByPath(out.stock_stamps);
    return std::string();
  }

  // `path` as a path under `version_dir` with '/' separators; the deriver may give it absolute or relative.
  static bool RelativeTo(const std::string& version_dir, std::string path, std::string* out) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string base = version_dir;
    std::replace(base.begin(), base.end(), '\\', '/');
    base += '/';
    if (path.compare(0, base.size(), base) == 0) {
      path.erase(0, base.size());
    } else if (path.empty() || path[0] == '/' || path.find(':') != std::string::npos) {
      return false;
    }
    if (path.empty() || path.find("..") != std::string::npos) return false;
    *out = path;
    return true;
  }

  bool SizeOf(const std::string& path, std::uint64_t* size) {
    std::int64_t handle = 0;
    if (!env_.OpenBlob(path, &handle, size)) return false;
    env_.CloseBlob(handle);
    return true;
  }

  // A zero stamp when the file system has no file id: the boot then defers with stamp_unsupported.
  bool Stamp(const std::string& path, FileStamp* stamp) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!fs_.OpenHeld(path, &handle, &size)) return false;
    if (!fs_.StampOf(handle, stamp)) *stamp = FileStamp();
    env_.CloseBlob(handle);
    return true;
  }

  // GETs `url` into memory, at most `cap` bytes. Returns a reason or "".
  std::string GetSmall(const std::string& url, std::size_t cap, std::string* out) {
    out->clear();
    long status = 0;
    bool too_large = false;
    const HttpResult http = env_.Get(
        url,
        [&](long st, const void* data, std::size_t length) {
          status = st;
          if (st != 200) return false;
          if (out->size() + length > cap) {
            too_large = true;
            return false;
          }
          out->append(static_cast<const char*>(data), length);
          return true;
        },
        keep_going_);
    if (http.cancelled) return "cancelled";
    if (too_large) return "over_cap";
    const long final_status = status != 0 ? status : http.status;
    if (final_status != 200) return HttpReason(final_status);
    if (!http.completed) return "transport";
    return std::string();
  }

  std::string FetchManifest(std::string* out) {
    return GetSmall(config_.base_url + kManifestUrlPath, kMaxSignedBytes, out);
  }

  // The package index blob, checked against its pin, with the piece retry rule.
  std::string FetchIndex(const Pin& pin, std::string* out) {
    if (pin.size == 0 || pin.size > kMaxIndexBytes) return "over_cap";
    std::string reason;
    for (int attempt = 0; attempt < kDownloadAttempts; ++attempt) {
      reason = GetSmall(config_.base_url + kBlobPrefix + pin.sha256, static_cast<std::size_t>(pin.size), out);
      if (reason == "cancelled" || reason == "over_cap") return reason;
      if (!reason.empty()) continue;
      std::unique_ptr<nevr_cargo::Hasher> hasher = env_.NewHasher();
      hasher->Update(out->data(), out->size());
      if (out->size() == pin.size && hasher->Hex() == pin.sha256) return std::string();
      reason = "index_hash";
    }
    return reason;
  }

  // Copies [offset, offset + size) of an open file into `temp`, feeding `member` as it goes. False on a short read or a
  // write failure, after which `temp` holds a partial copy.
  bool CopyRange(std::int64_t handle, std::uint64_t offset, std::uint64_t size, nevr_cargo::TempFile& temp,
                 nevr_cargo::Hasher& member) {
    for (std::uint64_t done = 0; done < size;) {
      const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(), size - done));
      if (!ReadExact(env_, handle, offset + done, buffer_.data(), length)) return false;
      member.Update(buffer_.data(), length);
      if (!temp.Write(buffer_.data(), length)) return false;
      done += length;
    }
    return true;
  }

  bool HashRange(std::int64_t handle, std::uint64_t offset, std::uint64_t size, std::string* sha256) {
    std::unique_ptr<nevr_cargo::Hasher> hasher = env_.NewHasher();
    for (std::uint64_t done = 0; done < size;) {
      const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(), size - done));
      if (!ReadExact(env_, handle, offset + done, buffer_.data(), length)) return false;
      hasher->Update(buffer_.data(), length);
      done += length;
    }
    *sha256 = hasher->Hex();
    return true;
  }

  // A piece some file on disk already holds: hashed in place first, so a copy into the member only starts once it is
  // known good. 1 = copied, 0 = not usable (fetch it), -1 = the copy failed after it started (the member is spoiled).
  int ReusePiece(const Pin& piece, nevr_cargo::TempFile& temp, nevr_cargo::Hasher& member) {
    const auto found = catalog_.find(piece.sha256);
    if (found == catalog_.end() || found->second.size != piece.size) return 0;
    const PieceLocation where = found->second;
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!env_.OpenBlob(catalog_files_[where.file], &handle, &size)) {
      catalog_.erase(found);
      return 0;
    }
    std::string sha;
    if (!Inside(where.offset, where.size, size) || !HashRange(handle, where.offset, where.size, &sha) ||
        sha != piece.sha256) {
      env_.CloseBlob(handle);
      catalog_.erase(found);
      return 0;
    }
    const bool copied = CopyRange(handle, where.offset, where.size, temp, member);
    env_.CloseBlob(handle);
    return copied ? 1 : -1;
  }

  // One GET of a piece into its own file under `pieces_dir`, renamed to its sha256 once the size and the hash match.
  Download GetPiece(const std::string& pieces_dir, const Pin& piece) {
    Download out;
    std::unique_ptr<nevr_cargo::TempFile> temp = env_.CreateTemp(pieces_dir);
    std::unique_ptr<nevr_cargo::Hasher> hasher = env_.NewHasher();
    if (!temp || !hasher) {
      out.reason = "temp";
      return out;
    }
    std::uint64_t got = 0;
    long status = 0;
    bool too_large = false, write_failed = false;
    const HttpResult http = env_.Get(
        config_.base_url + kBlobPrefix + piece.sha256,
        [&](long st, const void* data, std::size_t length) {
          status = st;
          if (st != 200) return false;
          if (got + length > piece.size) {
            too_large = true;
            return false;
          }
          hasher->Update(data, length);
          if (!temp->Write(data, length)) {
            write_failed = true;
            return false;
          }
          got += length;
          return true;
        },
        keep_going_);
    const long final_status = status != 0 ? status : http.status;
    if (http.cancelled) {
      out.reason = "cancelled";
    } else if (too_large) {
      out.reason = "over_cap";
    } else if (write_failed) {
      out.reason = "write_failed";
    } else if (final_status != 200) {
      out.reason = HttpReason(final_status);
      out.retry = true;
    } else if (!http.completed || got != piece.size || hasher->Hex() != piece.sha256) {
      out.reason = "piece_hash";
      out.retry = true;
    } else if (!temp->Commit(Join(pieces_dir, piece.sha256))) {
      out.reason = "commit_failed";
    }
    if (!out.reason.empty()) {
      temp->Abort();
      return out;
    }
    report_.bytes_fetched += got;
    return out;
  }

  // Concatenates the member's pieces, each reused from disk or fetched, into tmp/<member path>. Returns a reason or "".
  std::string AssembleMember(const std::string& tmp, const Member& member, std::size_t* fetched, std::size_t* reused) {
    const std::string path = Join(tmp, member.path);
    const std::string pieces_dir = Join(tmp, kPiecesDir);
    std::unique_ptr<nevr_cargo::TempFile> temp = env_.CreateTemp(ParentOf(path));
    std::unique_ptr<nevr_cargo::Hasher> hasher = env_.NewHasher();
    if (!temp || !hasher) return "temp";
    auto fail = [&](const std::string& reason) {
      temp->Abort();
      return reason;
    };
    for (const Pin& piece : member.pieces) {
      if (!keep_going_()) return fail("cancelled");
      const int reuse = ReusePiece(piece, *temp, *hasher);
      if (reuse < 0) return fail("read_failed");
      if (reuse > 0) {
        ++*reused;
        ++report_.pieces_reused;
        continue;
      }
      Download download;
      for (int attempt = 0; attempt < kDownloadAttempts; ++attempt) {
        download = GetPiece(pieces_dir, piece);
        if (!download.retry) break;
      }
      if (!download.reason.empty()) {
        env_.Event(EventLevel::kWarn, "map_piece", {{"sha256", piece.sha256}, {"reason", download.reason}});
        return fail(download.reason);
      }
      ++*fetched;
      ++report_.pieces_fetched;
      const std::string piece_path = Join(pieces_dir, piece.sha256);
      std::int64_t handle = 0;
      std::uint64_t size = 0;
      if (!env_.OpenBlob(piece_path, &handle, &size)) return fail("read_failed");
      const bool copied = size == piece.size && CopyRange(handle, 0, piece.size, *temp, *hasher);
      env_.CloseBlob(handle);
      if (!copied) return fail("read_failed");
      catalog_files_.push_back(piece_path);
      catalog_[piece.sha256] = PieceLocation{catalog_files_.size() - 1, 0, piece.size};
    }
    if (hasher->Hex() != member.sha256) {
      env_.Event(EventLevel::kWarn, "map_member", {{"path", member.path}, {"reason", "member_hash"}});
      return fail("member_hash");
    }
    if (!temp->Commit(path)) return "commit_failed";
    return std::string();
  }

  void PlanAndFetch() {
    const Manifest& manifest = parsed_.manifest;
    std::uint64_t used = 0;
    for (const std::string& id : subscribed_) {
      if (!keep_going_()) return;
      const std::string key = MapKey(id, config_.platform);
      auto plan = [&](const char* reason, EventLevel level = EventLevel::kInfo) {
        env_.Event(level, "map_plan", {{"id", id}, {"reason", reason}});
      };
      if (Contains(config_.local_maps, id)) {
        plan("local_map");
        continue;
      }
      if (Contains(manifest.revoked_ids, id)) {
        Drop(key);
        plan("revoked", EventLevel::kWarn);
        continue;
      }
      const PlatformEntry* listed = nullptr;
      const auto map = manifest.maps.find(id);
      if (map != manifest.maps.end()) {
        const auto platform = map->second.platforms.find(config_.platform);
        if (platform != map->second.platforms.end()) listed = &platform->second;
      }
      if (listed == nullptr) {
        plan("not_listed", EventLevel::kWarn);
        continue;
      }
      const PlatformEntry& entry = *listed;
      if (Contains(manifest.revoked_sha256, entry.index.sha256)) {
        Drop(key);
        plan("revoked", EventLevel::kWarn);
        continue;
      }
      if (!GameBuildOk(config_, entry.game_build)) {
        plan("unsupported_game", EventLevel::kWarn);
        continue;
      }
      if (entry.scripts > 0 && config_.signed_only) {
        plan("signed_only", EventLevel::kWarn);
        continue;
      }
      if (entry.scripts > 0 && config_.is_server && !ServerAllows(config_, id, entry.index.sha256)) {
        plan("server_not_allowed", EventLevel::kWarn);
        continue;
      }
      const auto cached = index_.maps.find(key);
      if (cached != index_.maps.end() && cached->second.index_sha256 == entry.index.sha256) {
        used += entry.bytes;
        plan("current");
        continue;
      }
      if (used + entry.bytes > config_.cap_bytes) {
        if (cached != index_.maps.end()) {
          for (const CachedFile& f : cached->second.files) used += f.size;
        }
        plan("over_cap", EventLevel::kWarn);
        continue;
      }
      used += entry.bytes;
      plan("fetch");
      FetchMap(id, entry);
    }
  }

  void FetchMap(const std::string& id, const PlatformEntry& entry) {
    const std::string key = MapKey(id, config_.platform);
    auto fail = [&](const char* event, const std::string& why) {
      env_.Event(why == "cancelled" ? EventLevel::kInfo : EventLevel::kWarn, event,
                 {{"id", id}, {"version", entry.version}, {"reason", why}});
    };
    std::string index_bytes;
    std::string reason = FetchIndex(entry.index, &index_bytes);
    if (!reason.empty()) return fail("map_index", reason);
    const ParsedPackage parsed = ParsePackageIndex(index_bytes, id, config_.platform, entry);
    if (!parsed.reason.empty()) return fail("map_index", parsed.reason);
    const Package& package = parsed.package;
    // The plan charged entry.bytes against the cap, so the members may not add up to more.
    std::uint64_t member_bytes = 0;
    for (const Member& m : package.members) member_bytes += m.size;
    if (member_bytes > entry.bytes) return fail("map_index", "bytes_mismatch");
    if (!RevokedSha(parsed_.manifest, entry.index.sha256, package).empty()) {
      Drop(key);
      return fail("map_plan", "revoked");
    }
    if (!StockManifestOk(package, config_)) return fail("map_plan", "stock_mismatch");
    const bool derive = NeedsDerive(package);
    if (derive && deriver_ == nullptr) return fail("map_derive", "derived_unsupported");

    const std::string rel = id + "/" + config_.platform + "/" + VersionDirName(entry.index.sha256);
    const std::string final_dir = Join(config_.dir, rel);
    const std::string tmp = final_dir + ".tmp";
    auto abandon = [&](const char* event, const std::string& why) {
      if (why != "cancelled") fs_.RemoveTree(tmp);
      fail(event, why);
    };
    if (!fs_.RemoveTree(tmp)) return fail("map_ready", "tmp_busy");
    if (!env_.MakeDirs(Join(tmp, kPiecesDir))) return abandon("map_ready", "mkdir");
    for (const Member& m : package.members) {
      if (!env_.MakeDirs(ParentOf(Join(tmp, m.path)))) return abandon("map_ready", "mkdir");
    }
    if (!env_.WriteFileAtomically(Join(tmp, kPackageFile), index_bytes)) return abandon("map_ready", "write_failed");

    CachedMap cached;
    cached.id = id;
    cached.platform = config_.platform;
    cached.version = entry.version;
    cached.index_sha256 = entry.index.sha256;
    cached.index_size = entry.index.size;
    cached.level = entry.level;
    cached.level_name = entry.level_name;
    cached.game_build = entry.game_build;
    cached.dir = rel;
    cached.pinned_serial = parsed_.manifest.serial;
    cached.stock_manifest_ok = true;
    std::size_t fetched = 0, reused = 0;
    for (const Member& m : package.members) {
      reason = AssembleMember(tmp, m, &fetched, &reused);
      if (!reason.empty()) return abandon("map_member", reason);
      if (m.role == Role::kRecipe) continue;
      CachedFile file;
      file.path = m.path;
      file.role = m.role == Role::kSet ? FileRole::kSet : FileRole::kScript;
      file.script_hash = m.script_hash;
      file.size = m.size;
      file.sha256 = m.sha256;
      if (m.role == Role::kSet) {
        std::string problem;
        file.frames_checked = WalkSetFrames(env_, Join(tmp, m.path), &problem);
        if (!file.frames_checked) env_.Event(EventLevel::kWarn, "map_member", {{"path", m.path}, {"frames", problem}});
      }
      cached.files.push_back(std::move(file));
    }
    if (derive) {
      reason = RunDerive(tmp, package, &cached.files, &cached.stock_stamps);
      if (!reason.empty()) return abandon("map_derive", reason);
    }
    if (!fs_.RemoveTree(Join(tmp, kPiecesDir))) return abandon("map_ready", "tmp_busy");
    if (!fs_.RemoveTree(final_dir)) return abandon("map_ready", "dir_busy");
    if (!fs_.RenameDir(tmp, final_dir)) return abandon("map_ready", "rename");
    for (CachedFile& f : cached.files) {
      if (!Stamp(Join(final_dir, f.path), &f.stamp)) return fail("map_ready", "stamp");
    }
    index_.maps[key] = std::move(cached);
    dirty_ = true;
    ++report_.maps_updated;
    AddToCatalog(final_dir, package);
    env_.Event(EventLevel::kInfo, "map_ready",
               {{"id", id}, {"version", entry.version}, {"pieces_fetched", fetched}, {"pieces_reused", reused}});
  }

  Env& env_;
  MapFs& fs_;
  const ChannelConfig& config_;
  Deriver* deriver_;
  const std::function<bool()>& keep_going_;
  std::vector<char> buffer_;
  CacheIndex index_;
  ParsedManifest parsed_;
  std::vector<std::string> subscribed_;
  std::vector<std::string> catalog_files_;
  std::unordered_map<std::string, PieceLocation> catalog_;
  bool dirty_ = false;
  FetchReport report_;
};

}  // namespace

bool WalkSetFrames(Env& env, const std::string& path, std::string* problem) {
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  if (!env.OpenBlob(path, &handle, &size)) {
    *problem = "open";
    return false;
  }
  const bool ok = WalkOpenSet(env, handle, size, problem);
  env.CloseBlob(handle);
  return ok;
}

BootReport BootMount(Env& env, MapFs& fs, const ChannelConfig& config, Mounter& mounter, Deriver* deriver,
                     const std::string& now_utc) {
  return BootRun(env, fs, config, mounter, deriver).Run(now_utc);
}

FetchReport FetchAll(Env& env, MapFs& fs, const ChannelConfig& config, const std::vector<std::string>& deferred,
                     Deriver* deriver, const std::string& now_utc, const std::function<bool()>& keep_going) {
  return FetchRun(env, fs, config, deriver, keep_going).Run(deferred, now_utc);
}

}  // namespace nevr_maps
