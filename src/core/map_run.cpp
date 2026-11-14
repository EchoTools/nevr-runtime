#include "core/map_run.h"

#include <algorithm>
#include <memory>
#include <set>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/cargo_format.h"
#include "core/cargo_limits.h"
#include "core/device_memory.h"
#include "core/map_index.h"
#include "core/map_set_tables.h"
#include "nevr/symbol_hash.h"

namespace nevr_maps {
namespace {

using nevr_cargo::Env;
using nevr_cargo::EventLevel;
using nevr_cargo::Field;
using nevr_cargo::HttpResult;
using json = nlohmann::json;

// A GET of a piece or an index is tried this many times when it fails in transport or its bytes do not match: the first try and
// three retries.
constexpr int kDownloadAttempts = 4;
// One read of a stored file when a piece is copied or a deferred map is hashed again: at most this many bytes, and never more than
// the device's share for one object (cargo_limits.h, Limits::max_object_bytes).
constexpr std::uint64_t kCopyStepBytes = std::uint64_t{1} << 20;
// What one listed map costs the fetch in memory, for the entry share of nevr_cargo::ComputeLimits.
constexpr std::uint64_t kMapEntryMemoryBytes = sizeof(MapEntry) + sizeof(PlatformEntry) + sizeof(CachedMap);

std::string Join(const std::string& a, const std::string& b) { return a + "/" + b; }

std::string Owner(const std::string& id) { return "map:" + id; }

std::string ParentOf(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string HttpReason(long status) { return status == 0 ? "transport" : "http_" + std::to_string(status); }

// The transport may ask with a range from byte 0, which a CDN answers 206 with the whole object; a server that ignores a range
// answers 200. Both carry the object from its first byte (core/cargo_run.cpp reads them the same way).
bool IsWholeBody(long status) { return status == nevr_cargo::kHttpOk || status == nevr_cargo::kHttpPartialContent; }

// A failure that leaves the verified pieces worth keeping: the next fetch resumes after them.
bool Resumable(const std::string& reason) {
  return reason == "cancelled" || reason == "transport" || reason.rfind("http_", 0) == 0;
}

bool Contains(const std::vector<std::string>& list, const std::string& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

bool IsStagingName(const std::string& name) {
  const std::string suffix = kStagingSuffix;
  return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ExpectedChannel(const ChannelConfig& config, const FloorBook& floors) {
  if (!config.channel.empty()) return config.channel;
  const auto bound = floors.bound.find(config.base_url);
  return bound != floors.bound.end() ? bound->second : std::string();
}

// The most bytes of a signed group manifest: the signature part, the separator line and a body this device may read.
std::uint64_t MaxSignedBytes(std::uint64_t document_bytes) {
  return std::min(kMaxManifestBytes, document_bytes) + kMaxMinisigBytes + kSignedSeparator.size();
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
std::string RevokedSha(const std::set<std::string>& revoked, const std::string& index_sha256, const Package& package) {
  if (revoked.empty()) return std::string();
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

std::string JoinList(const std::vector<std::string>& list) {
  std::string text;
  for (const std::string& item : list) text += (text.empty() ? "" : ",") + item;
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

bool Inside(std::uint64_t offset, std::uint64_t length, std::uint64_t total) {
  return offset <= total && length <= total - offset;
}

bool ReadExact(Env& env, std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length) {
  std::size_t got = 0;
  return env.ReadBlob(handle, offset, buffer, length, &got) && got == length;
}

// The memory shares a pass takes from the device (ADR 0045 decision 8). A fact that is missing stops the pass with its name.
nevr_cargo::Limits LoadLimits(Env& env, const std::string& dir, const char* source, nevr_cargo::DeviceFacts* facts) {
  env.QueryDevice(dir, facts);
  const nevr_cargo::Limits limits = nevr_cargo::ComputeLimits(*facts, kMapEntryMemoryBytes);
  if (limits.ok) {
    env.Event(EventLevel::kDebug, "map_limits",
              {{"source", source},
               {"result", "ok"},
               {"storage_budget", limits.storage_budget},
               {"max_object_bytes", limits.max_object_bytes},
               {"document_bytes", MapDocumentBytes(*facts)}});
  } else {
    env.Event(EventLevel::kWarn, "map_limits",
              {{"source", source},
               {"result", "refused"},
               {"reason", limits.reason},
               {"consequence", "no map is fetched or mounted this pass; the device did not give the fact it names"}});
  }
  return limits;
}

std::size_t CopyBufferBytes(const nevr_cargo::Limits& limits) {
  return static_cast<std::size_t>(std::max<std::uint64_t>(1, std::min(kCopyStepBytes, limits.max_object_bytes)));
}

// ---------------------------------------------------------------------------------------------------------------------------
// The mount.

// Every file one cached map mounts, as the mount opens them.
struct Opened {
  const CachedFile* file = nullptr;
  std::int64_t handle = 0;
  bool granted = false;  // the mounter owns the handle
};

// One group's listing of a map, in the order the groups were given (the latest first).
struct Listing {
  std::string group;
  PlatformEntry entry;
  std::string title;
  bool official = false;
  bool expired = false;
};

class MountRun {
 public:
  MountRun(Env& env, MapFs& fs, const ChannelConfig& config, Mounter& mounter, Deriver* deriver,
           std::map<std::string, std::string>& mounted, std::map<std::uint64_t, std::string>& script_holders)
      : env_(env), fs_(fs), config_(config), mounter_(mounter), deriver_(deriver), mounted_(mounted),
        script_holders_(script_holders) {}

  MountReport Run(const std::vector<std::string>& groups, const std::string& now_utc) {
    const std::uint64_t started = env_.NowMs();
    nevr_cargo::DeviceFacts facts;
    env_.QueryDevice(config_.dir, &facts);
    if (!facts.have_fds) return Stop("no_descriptor_fact");
    if (!facts.have_memory) return Stop("no_memory_fact");
    const std::uint64_t free_fds = facts.fd_soft_limit > facts.fds_open ? facts.fd_soft_limit - facts.fds_open : 0;
    descriptor_budget_ = nevr_device::Share(free_fds, kDescriptorShareForMaps);
    document_bytes_ = MapDocumentBytes(facts);
    local_names_ = CollectLocalNames(config_.local_level_names, config_.local_maps);

    const CacheIndexStatus status = ReadCacheIndex(env_, config_.dir, &index_);
    if (status != CacheIndexStatus::kOk) {
      env_.Event(status == CacheIndexStatus::kAbsent ? EventLevel::kInfo : EventLevel::kWarn, "map_mount_group",
                 {{"reason", "no_index"}, {"index", CacheIndexStatusName(status)}});
      Summary();
      return std::move(report_);
    }
    std::set<std::string> seen;
    for (const std::string& group : groups) {
      if (seen.insert(group).second) LoadGroup(group, now_utc);
    }
    for (const auto& [id, listings] : listings_) {
      if (!revoked_ids_.count(id)) report_.listed.emplace_back(EchoVR::CSymbol64Hash(id.c_str()), id);
    }
    for (const auto& [id, listings] : listings_) {
      if (config_.boot_budget_ms != 0 && env_.NowMs() - started >= config_.boot_budget_ms) {
        Skip(id, "boot_budget");
        continue;
      }
      MountOne(id, listings);
    }
    Summary();
    return std::move(report_);
  }

 private:
  MountReport Stop(const char* reason) {
    env_.Event(EventLevel::kWarn, "map_mount_group",
               {{"reason", reason}, {"consequence", "no map is mounted; the device did not give the fact this names"}});
    Summary();
    return std::move(report_);
  }

  // A group's stored manifest, verified again against the floors and `now`; its listings join each map's, after those of the
  // groups given before it.
  void LoadGroup(const std::string& group, const std::string& now_utc) {
    auto refuse = [&](const std::string& reason) {
      report_.groups_refused.emplace_back(group, reason);
      env_.Event(EventLevel::kWarn, "map_mount_group",
                 {{"group", group}, {"reason", reason}, {"consequence", "no map of this group is mounted until its next fetch"}});
    };
    std::string signed_text;
    if (!IsPathSafeGroupId(group)) return refuse("bad_group");
    if (!env_.ReadSmallFile(StoredGroupManifestPath(config_.dir, group), &signed_text,
                            static_cast<std::size_t>(MaxSignedBytes(document_bytes_)))) {
      return refuse("no_manifest");
    }
    const ParsedManifest parsed = ParseAndVerifyManifest(signed_text, config_.keys, index_.floors,
                                                         ExpectedChannel(config_, index_.floors), now_utc, document_bytes_);
    if (!parsed.reason.empty()) return refuse(parsed.reason);
    if (parsed.guild != group) return refuse("guild_mismatch");
    const bool expired = IsExpired(parsed.manifest.expires, now_utc);
    for (const std::string& id : parsed.manifest.revoked_ids) revoked_ids_.insert(id);
    for (const std::string& sha : parsed.manifest.revoked_sha256) revoked_sha_.insert(sha);
    std::size_t listed = 0;
    for (const auto& [id, map] : parsed.manifest.maps) {
      const auto platform = map.platforms.find(config_.platform);
      if (platform == map.platforms.end()) continue;
      ++listed;
      Listing l;
      l.group = group;
      l.entry = platform->second;
      l.title = map.title;
      l.official = parsed.official;
      l.expired = expired;
      listings_[id].push_back(l);
    }
    env_.Event(EventLevel::kInfo, "map_mount_group",
               {{"group", group}, {"serial", parsed.manifest.serial}, {"channel", parsed.channel},
                {"expired", expired ? "true" : "false"}, {"official", parsed.official ? "true" : "false"}, {"maps", listed}});
  }

  void NotMounted(std::vector<std::string>* list, const std::string& id, const std::string& token) {
    list->push_back(id);
    report_.not_mounted.emplace_back(id, token);
  }

  // `local_name` is set for a local copy (kLocalMapToken): the local level or id that won.
  void Skip(const std::string& id, const std::string& token, const std::string& local_name = std::string()) {
    NotMounted(&report_.skipped, id, token);
    std::vector<Field> fields{{"id", id}, {"reason", token}};
    if (!local_name.empty()) fields.emplace_back("local_name", local_name);
    env_.Event(EventLevel::kWarn, "map_skipped", fields);
  }

  void Defer(const std::string& id, const std::string& token) {
    NotMounted(&report_.deferred, id, token);
    env_.Event(EventLevel::kWarn, "map_deferred",
               {{"id", id}, {"reason", token}, {"consequence", "the map is hashed again at the next fetch"}});
  }

  void Refuse(const std::string& id, const std::string& token, const std::string& holder = std::string()) {
    report_.refused.emplace_back(id, token);
    report_.not_mounted.emplace_back(id, token);
    std::vector<Field> fields{{"id", id}, {"refused", token}};
    if (!holder.empty()) fields.emplace_back("holder", Owner(holder));
    env_.Event(EventLevel::kWarn, "map_mounted", fields);
  }

  // Once per distinct (path, stamp) per call, however many maps' recipes read that stock file.
  bool StockCurrent(const std::vector<StockStamp>& stamps) {
    for (const StockStamp& s : stamps) {
      const std::string key = s.path + '\n' + std::to_string(s.stamp.volume) + ':' + std::to_string(s.stamp.id_hi) + ':' +
                              std::to_string(s.stamp.id_lo) + ':' + std::to_string(s.stamp.size) + ':' +
                              std::to_string(s.stamp.last_write);
      auto known = stock_checked_.find(key);
      if (known == stock_checked_.end()) known = stock_checked_.emplace(key, deriver_->StockCurrent({s})).first;
      if (!known->second) return false;
    }
    return true;
  }

  // The package index at `path`, read through one held handle whose stamp equals `recorded`: "" with the bytes, or why not. A
  // `recorded` never taken is all zero, which no stamp read from a file equals (its volume, id and size), so it counts as moved.
  std::string ReadIndexByStamp(const std::string& path, const FileStamp& recorded, std::string* out) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!fs_.OpenHeld(path, &handle, &size)) return "missing";
    FileStamp stamp;
    std::string why;
    if (!fs_.StampOf(handle, &stamp)) {
      why = "stamp_unsupported";
    } else if (stamp != recorded) {
      why = "package_stamp";
    } else if (size > std::min(kMaxIndexBytes, document_bytes_)) {
      why = "too_large";
    } else {
      out->assign(static_cast<std::size_t>(size), '\0');
      if (size != 0 && !ReadExact(env_, handle, 0, &(*out)[0], out->size())) why = "missing";
    }
    env_.CloseBlob(handle);
    return why;
  }

  void CloseUngranted(const std::vector<Opened>& opened) {
    for (const Opened& o : opened) {
      if (!o.granted) env_.CloseBlob(o.handle);
    }
  }

  // The listing whose manifest pins the version the cache holds, the first such group in the order given. When none pins it
  // (the manifest that did was replaced, and the new version is not stored yet), the cached version still mounts under the first
  // group that lists the map, but its scripts are not counted as official: no manifest verified now vouches for them.
  static Listing Choose(const std::vector<Listing>& listings, const std::string& cached_sha256) {
    for (const Listing& l : listings) {
      if (l.entry.index.sha256 == cached_sha256) return l;
    }
    Listing fallback = listings.front();
    fallback.official = false;
    return fallback;
  }

  void MountOne(const std::string& id, const std::vector<Listing>& listings) {
    // ADR 0048 decision 8: the CDN map's names (its id and each level name its listings carry) against this run's local names.
    std::vector<std::string> cdn_names{id};
    for (const Listing& l : listings) cdn_names.push_back(l.entry.level_name);
    const LocalPrecedence local = DecideLocalPrecedence(cdn_names, local_names_);
    if (local.skip) return Skip(id, kLocalMapToken, local.matched);
    if (revoked_ids_.count(id)) return Refuse(id, "revoked");
    const auto found = index_.maps.find(MapKey(id, config_.platform));
    if (found == index_.maps.end() || found->second.pinned_serial == 0) {
      // A fetch drops a version a manifest revoked: the mount says why the map is gone.
      const bool revoked = std::any_of(listings.begin(), listings.end(),
                                       [&](const Listing& l) { return revoked_sha_.count(l.entry.index.sha256) != 0; });
      return revoked ? Refuse(id, "revoked") : Skip(id, "not_cached");
    }
    const CachedMap& cached = found->second;
    const auto earlier = mounted_.find(id);
    if (earlier != mounted_.end()) {
      if (earlier->second == cached.index_sha256) {
        report_.kept.push_back(id);
        return;
      }
      return Skip(id, "mounted_other_version");
    }
    if (revoked_sha_.count(cached.index_sha256)) return Refuse(id, "revoked");
    const Listing selected = Choose(listings, cached.index_sha256);
    const std::string version_dir = Join(config_.dir, cached.dir);

    // The package index is held to its pin by its stamp, never hashed here: while the file's stamp, read through the handle the
    // bytes are read from, equals the one recorded when the fetch hashed them, they are those bytes (ADR 0048 decision 4). A moved
    // stamp defers the map to the worker, which hashes the index again (Rehash).
    std::string package_bytes;
    const std::string unread = ReadIndexByStamp(Join(version_dir, kPackageFile), cached.index_stamp, &package_bytes);
    if (unread == "too_large") return Refuse(id, unread);
    if (!unread.empty()) return Defer(id, unread);
    const ParsedPackage parsed =
        ParsePackageIndex(package_bytes, id, config_.platform, EntryOf(cached), document_bytes_, IndexPin::kStamped);
    if (!parsed.reason.empty()) return Refuse(id, parsed.reason);
    const Package& package = parsed.package;

    if (!RevokedSha(revoked_sha_, cached.index_sha256, package).empty()) return Refuse(id, "revoked");
    if (!GameBuildOk(config_, cached.game_build)) return Refuse(id, "unsupported_game");
    if (!StockManifestOk(package, config_)) return Refuse(id, "stock_mismatch");
    const bool scripts = HasScripts(package);
    if (scripts && selected.expired) return Refuse(id, "expired");
    if (scripts && config_.signed_only && !selected.official) return Refuse(id, "signed_only");
    const bool derive = NeedsDerive(package);
    if ((derive || !cached.stock_stamps.empty()) && deriver_ == nullptr) return Refuse(id, "derived_unsupported");
    if (!cached.stock_stamps.empty() && !StockCurrent(cached.stock_stamps)) return Defer(id, "stock_changed");

    // The files this map mounts: the set (the merged one when recipes rebuilt it), the published scripts, the rebuilt ones.
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

    std::vector<const CachedFile*> wanted{set};
    for (const auto& [hash, file] : published) wanted.push_back(file);
    for (const RekeyPin* r : derived_scripts) wanted.push_back(rebuilt[r->hash]);
    // Every file is held for the life of the process: the device's descriptor share bounds how many the maps take.
    if (held_ + wanted.size() > descriptor_budget_) return Skip(id, "descriptors");

    // Open and hold every file first, so a stamp that moved defers the map before anything is granted.
    std::vector<Opened> opened;
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
    const std::string alias_refusal = StageAliasesOrWithdraw(mounter_, owner, id, package.level, package.config_aliases);
    if (!alias_refusal.empty()) return Refuse(id, alias_refusal);
    for (std::size_t i = 1; i < opened.size(); ++i) script_holders_[opened[i].file->script_hash] = id;
    for (const RekeyPin* r : rekeys) script_holders_[r->hash] = id;
    held_ += opened.size();
    mounted_[id] = cached.index_sha256;
    report_.mounted.push_back(id);
    MountedMap staged;
    staged.id = id;
    staged.version = cached.version;
    staged.index_sha256 = cached.index_sha256;
    staged.platform = config_.platform;
    staged.source_sha256 = package.source_sha256;
    staged.title = selected.title;
    staged.group = selected.group;
    staged.level = package.level;
    report_.staged.push_back(std::move(staged));
    env_.Event(EventLevel::kInfo, "map_mounted",
               {{"id", id},
                {"version", cached.version},
                {"group", selected.group},
                {"scripts", opened.size() - 1 + rekeys.size()},
                {"verified_stamp", set->frames_checked ? "true" : "false"}});
  }

  void Summary() {
    std::vector<std::pair<std::string, std::string>> skipped, deferred;
    for (const auto& [id, token] : report_.not_mounted) {
      if (Contains(report_.skipped, id)) skipped.emplace_back(id, token);
      if (Contains(report_.deferred, id)) deferred.emplace_back(id, token);
    }
    const bool warn = !report_.refused.empty() || !skipped.empty() || !deferred.empty() || !report_.groups_refused.empty();
    env_.Event(warn ? EventLevel::kWarn : EventLevel::kInfo, "map_summary",
               {{"mounted", report_.mounted.size()},
                {"kept", report_.kept.size()},
                {"refused", JoinPairs(report_.refused)},
                {"skipped", JoinPairs(skipped)},
                {"deferred", JoinPairs(deferred)},
                {"groups_refused", JoinPairs(report_.groups_refused)}});
  }

  Env& env_;
  MapFs& fs_;
  const ChannelConfig& config_;
  Mounter& mounter_;
  Deriver* deriver_;
  std::map<std::string, std::string>& mounted_;
  std::map<std::uint64_t, std::string>& script_holders_;
  CacheIndex index_;
  std::map<std::string, std::vector<Listing>> listings_;
  std::set<std::string> revoked_ids_, revoked_sha_;
  std::set<std::string> local_names_;  // this run's local level names and local map ids (core/map_local_precedence.h)
  MountReport report_;
  std::map<std::string, bool> stock_checked_;
  std::uint64_t descriptor_budget_ = 0, document_bytes_ = 0, held_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------
// The fetch.

// Where a piece's bytes lie in a file the cache holds. A file of a version directory carries the stamp it had when its bytes were
// verified; a piece is copied from it only while the stamp is the same.
struct PieceLocation {
  std::string path;
  std::uint64_t offset = 0, size = 0;
  bool stamped = false;
  FileStamp stamp;
};

struct Download {
  std::string reason;  // "" on success
  bool retry = false;
};

// What a stopped transfer verified of one member: its first `pieces` pieces, `bytes` long, and the file's stamp when it stopped.
struct ResumePoint {
  std::uint64_t pieces = 0, bytes = 0;
  FileStamp stamp;
};

class FetchRun {
 public:
  FetchRun(Env& env, MapFs& fs, const ChannelConfig& config, const std::string& group, Deriver* deriver,
           const std::function<bool()>& keep_going, const FetchProgressFn& progress)
      : env_(env), fs_(fs), config_(config), group_(group), deriver_(deriver), keep_going_(keep_going),
        progress_fn_(progress) {
    report_.group = group;
    local_names_ = CollectLocalNames(config.local_level_names, config.local_maps);
  }

  FetchReport Run(const std::vector<std::string>& deferred, const std::string& now_utc) {
    started_ = env_.NowMs();
    if (!IsPathSafeGroupId(group_)) return Finish("bad_group");
    if (!env_.MakeDirs(config_.dir)) return Finish("cache_dir");
    nevr_cargo::DeviceFacts facts;
    limits_ = LoadLimits(env_, config_.dir, "fetch", &facts);
    if (!limits_.ok) return Finish(limits_.reason);
    document_bytes_ = MapDocumentBytes(facts);
    table_bytes_ = nevr_device::Share(facts.memory_available, nevr_cargo::kMemoryShareForEntries);
    buffer_.resize(CopyBufferBytes(limits_));

    const CacheIndexStatus status = ReadCacheIndex(env_, config_.dir, &index_);
    if (status != CacheIndexStatus::kOk && status != CacheIndexStatus::kAbsent) {
      env_.Event(EventLevel::kWarn, "map_index",
                 {{"reason", CacheIndexStatusName(status)}, {"consequence", "the cache is treated as empty"}});
    }
    RememberGroup();
    BoundGroups();
    CollectGarbage();
    BuildCatalog();
    Rehash(deferred);
    if (!keep_going_()) return Finish("cancelled");
    if (config_.base_url.empty()) return Finish("no_base_url");

    std::string signed_text;
    std::string failure = GetSmall(GroupManifestUrl(config_.base_url, group_), MaxSignedBytes(document_bytes_), &signed_text);
    if (!failure.empty()) {
      env_.Event(failure == "cancelled" ? EventLevel::kInfo : EventLevel::kWarn, "map_manifest",
                 {{"group", group_}, {"reason", failure}, {"consequence", "the stored manifest is kept"}});
      return Finish(failure);
    }
    parsed_ = ParseAndVerifyManifest(signed_text, config_.keys, index_.floors, ExpectedChannel(config_, index_.floors), now_utc,
                                     document_bytes_);
    if (parsed_.reason.empty() && parsed_.guild != group_) parsed_.reason = "guild_mismatch";
    if (!parsed_.reason.empty()) {
      env_.Event(EventLevel::kWarn, "map_manifest",
                 {{"group", group_}, {"reason", parsed_.reason}, {"consequence", "the stored manifest is kept"}});
      return Finish(parsed_.reason);
    }
    for (const auto& [key, reason] : parsed_.dropped) {
      env_.Event(EventLevel::kWarn, "map_manifest", {{"group", group_}, {"dropped", key}, {"reason", reason}});
    }
    env_.Event(EventLevel::kInfo, "map_manifest",
               {{"group", group_},
                {"serial", parsed_.manifest.serial},
                {"channel", parsed_.channel},
                {"maps", parsed_.manifest.maps.size()}});
    LogTrustChange();
    revoked_sha_.insert(parsed_.manifest.revoked_sha256.begin(), parsed_.manifest.revoked_sha256.end());

    PlanAndFetch();
    if (keep_going_()) RemoveSupersededStaging();

    // The commit order: the manifest, then the floors it raises, then the index naming what is in place, last.
    std::string reason = keep_going_() ? std::string() : "cancelled";
    const std::string stored = StoredGroupManifestPath(config_.dir, group_);
    if (!env_.MakeDirs(ParentOf(stored)) || !env_.WriteFileAtomically(stored, signed_text)) {
      reason = "manifest_write";
    } else {
      index_.floors = AdvanceFloors(index_.floors, config_.base_url, parsed_);
      RecordGroupMaps(signed_text.size());
    }
    return Finish(reason);
  }

 private:
  FetchReport Finish(const std::string& reason) {
    std::string why = reason;
    // The index records what is in place whatever ended the pass, so a stopped pass keeps its group's list and its staging.
    if (dirty_ && !WriteCacheIndex(env_, config_.dir, index_) && why.empty()) why = "index_write";
    if (why.empty() && !first_map_failure_.empty()) why = first_map_failure_;
    report_.reason = why;
    report_.outcome = !why.empty() ? FetchOutcome::kFailed
                                   : (report_.maps_updated > 0 ? FetchOutcome::kFetched : FetchOutcome::kCached);
    report_.elapsed_ms = env_.NowMs() - started_;
    env_.Event(why.empty() || why == "cancelled" ? EventLevel::kInfo : EventLevel::kWarn, "map_done",
               {{"group", group_},
                {"outcome", FetchOutcomeName(report_.outcome)},
                {"reason", why.empty() ? "ok" : why},
                {"maps_updated", report_.maps_updated},
                {"pieces_fetched", report_.pieces_fetched},
                {"pieces_reused", report_.pieces_reused},
                {"bytes_fetched", report_.bytes_fetched},
                {"set_bytes", report_.set_bytes},
                {"elapsed_ms", report_.elapsed_ms}});
    return report_;
  }

  void Progress() {
    if (!progress_fn_) return;
    FetchProgress p;
    p.group = group_;
    p.set_bytes = report_.set_bytes;
    p.bytes_to_fetch = report_.bytes_to_fetch;
    p.bytes_fetched = report_.bytes_fetched;
    p.elapsed_ms = env_.NowMs() - started_;
    progress_fn_(p);
  }

  // The requested group becomes the latest the index keeps.
  void RememberGroup() {
    auto& groups = index_.groups;
    const auto it = std::find_if(groups.begin(), groups.end(), [&](const CachedGroup& g) { return g.id == group_; });
    CachedGroup group;
    group.id = group_;
    if (it != groups.end()) {
      group = *it;
      groups.erase(it);
    }
    groups.insert(groups.begin(), group);
    dirty_ = true;
  }

  // The groups kept are as many, the latest first, as the cache's bytes and the device's storage share hold (ADR 0045 decision
  // 8): what the cache holds now plus what it may still add. The latest group is always kept. A map two groups list counts for
  // each, so the bound errs toward keeping fewer.
  void BoundGroups() {
    std::uint64_t cached = 0;
    for (const auto& [key, map] : index_.maps) {
      for (const CachedFile& f : map.files) cached += f.size;
    }
    const std::uint64_t budget = cached + limits_.storage_budget;
    std::uint64_t total = 0;
    bool full = false;
    std::vector<CachedGroup> kept;
    for (std::size_t i = 0; i < index_.groups.size(); ++i) {
      const CachedGroup& g = index_.groups[i];
      // The oldest go first: once a group does not fit, no older one is kept.
      full = full || (i != 0 && g.bytes > budget - std::min(total, budget));
      if (!full) {
        total += g.bytes;
        kept.push_back(g);
        continue;
      }
      env_.Event(EventLevel::kInfo, "map_gc",
                 {{"group", g.id}, {"reason", "storage"}, {"bytes", g.bytes}, {"budget", budget},
                  {"consequence", "the group's maps that no kept group lists are removed"}});
    }
    if (kept.size() != index_.groups.size()) {
      index_.groups = std::move(kept);
      dirty_ = true;
    }
  }

  // After a verified manifest: the maps the group lists for this platform, and what keeping it costs.
  void RecordGroupMaps(std::uint64_t manifest_bytes) {
    for (CachedGroup& g : index_.groups) {
      if (g.id != group_) continue;
      g.maps.clear();
      g.bytes = manifest_bytes;
      for (const auto& [id, map] : parsed_.manifest.maps) {
        const auto entry = map.platforms.find(config_.platform);
        if (entry == map.platforms.end()) continue;
        g.maps.push_back(MapKey(id, config_.platform));
        g.bytes += entry->second.bytes;
      }
    }
    dirty_ = true;
  }

  // Once, when a key first signs a group's manifest that keys no longer trusted have floors on (a test key to the official key).
  void LogTrustChange() {
    if (parsed_.untrusted_floor_keys.empty()) return;
    const auto channel = index_.floors.channels.find(parsed_.channel);
    if (channel != index_.floors.channels.end()) {
      const auto guild = channel->second.find(parsed_.guild);
      if (guild != channel->second.end() && guild->second.count(parsed_.key_id) != 0) return;
    }
    env_.Event(EventLevel::kWarn, "map_trust_changed",
               {{"channel", parsed_.channel},
                {"group", group_},
                {"key_id", parsed_.key_id},
                {"untrusted_keys", JoinList(parsed_.untrusted_floor_keys)},
                {"consequence", "this key starts its own replay floor; the untrusted keys' floors are ignored"}});
  }

  void Drop(const std::string& key) {
    if (index_.maps.erase(key)) dirty_ = true;
  }

  void Remove(const std::string& path, const std::string& what, const char* reason) {
    const bool removed = fs_.RemoveTree(path);
    env_.Event(removed ? EventLevel::kInfo : EventLevel::kWarn, "map_gc",
               {{"removed", what}, {"reason", reason}, {"result", removed ? "ok" : "held"}});
  }

  // Maps no kept group lists, other platforms, and version directories the index does not name are removed; so are the stored
  // manifests of groups no longer kept. An index entry whose directory is gone is dropped. A directory a mount holds open fails
  // to go and is tried again at the next fetch. A staging directory stays until RemoveSupersededStaging.
  void CollectGarbage() {
    std::set<std::string> listed;
    for (const CachedGroup& g : index_.groups) listed.insert(g.maps.begin(), g.maps.end());
    std::set<std::string> kept_groups;
    for (const CachedGroup& g : index_.groups) kept_groups.insert(g.id);

    std::vector<std::string> stored;
    const std::string manifests_dir = Join(config_.dir, kGroupManifestsDir);
    if (fs_.ListDir(manifests_dir, &stored)) {
      for (const std::string& group : stored) {
        if (!kept_groups.count(group)) Remove(Join(manifests_dir, group), group, "group_not_kept");
      }
    }

    std::vector<std::string> names;
    if (!fs_.ListDir(config_.dir, &names)) return;
    std::set<std::string> present;
    for (const std::string& id : names) {
      if (!IsValidMapId(id)) continue;  // index.json, guild-manifests, *.bad, and anything not ours
      const std::string id_dir = Join(config_.dir, id);
      if (!listed.count(MapKey(id, config_.platform))) {
        Remove(id_dir, id, "unlisted");
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
          } else if (!IsStagingName(version)) {
            Remove(Join(platform_dir, version), rel, "unreferenced");
          }
        }
      }
    }
    for (auto it = index_.maps.begin(); it != index_.maps.end();) {
      if (!present.count(it->first)) {
        env_.Event(EventLevel::kInfo, "map_gc", {{"dropped", it->first}, {"reason", "missing_or_unlisted"}});
        it = index_.maps.erase(it);
        dirty_ = true;
      } else {
        ++it;
      }
    }
  }

  // After a pass that ran to its end: every staging directory of a map this group lists is the version its manifest pins, kept to
  // resume a transfer that failed in transport, or one it no longer pins, which goes.
  void RemoveSupersededStaging() {
    for (const auto& [id, map] : parsed_.manifest.maps) {
      const auto entry = map.platforms.find(config_.platform);
      if (entry == map.platforms.end()) continue;
      const std::string platform_dir = Join(Join(config_.dir, id), config_.platform);
      std::vector<std::string> names;
      if (!fs_.ListDir(platform_dir, &names)) continue;
      const std::string pinned = VersionDirName(entry->second.index.sha256) + kStagingSuffix;
      for (const std::string& name : names) {
        if (IsStagingName(name) && name != pinned) {
          Remove(Join(platform_dir, name), id + "/" + config_.platform + "/" + name, "superseded");
        }
      }
    }
  }

  bool ReadPackage(const CachedMap& cached, Package* out) {
    std::string bytes;
    if (!env_.ReadSmallFile(Join(Join(config_.dir, cached.dir), kPackageFile), &bytes,
                            static_cast<std::size_t>(std::min(kMaxIndexBytes, document_bytes_)))) {
      return false;
    }
    ParsedPackage parsed = ParsePackageIndex(bytes, cached.id, cached.platform, EntryOf(cached), document_bytes_);
    if (!parsed.reason.empty()) return false;
    *out = std::move(parsed.package);
    return true;
  }

  // Every piece of every member of a cached version: where it lies and the stamp its file had when verified. A later location
  // replaces an earlier one, so a piece this pass stored is found in its final directory.
  void AddToCatalog(const std::string& version_dir, const Package& package, const std::vector<CachedFile>& files) {
    for (const Member& m : package.members) {
      const auto file = std::find_if(files.begin(), files.end(), [&](const CachedFile& f) { return f.path == m.path; });
      if (file == files.end()) continue;  // a recipe member is kept for the rebuild only, with no stamp
      std::uint64_t offset = 0;
      for (const Pin& p : m.pieces) {
        PieceLocation where;
        where.path = Join(version_dir, m.path);
        where.offset = offset;
        where.size = p.size;
        where.stamped = true;
        where.stamp = file->stamp;
        catalog_[p.sha256] = where;
        offset += p.size;
      }
    }
  }

  void BuildCatalog() {
    for (const auto& [key, cached] : index_.maps) {
      Package package;
      if (ReadPackage(cached, &package)) AddToCatalog(Join(config_.dir, cached.dir), package, cached.files);
    }
  }

  // The one place a stored file is hashed: a map a mount deferred (ADR 0048 decision 4), on the worker.
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

  // The mount deferred these maps: equal bytes refresh the stamps, other bytes drop the entry so the plan fetches it again. A map
  // whose stock files changed is rebuilt here, from its cached members, even though its index is unchanged.
  void Rehash(const std::vector<std::string>& deferred) {
    std::vector<std::string> ids(deferred);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const std::string& id : ids) {
      if (!keep_going_()) return;
      const auto found = index_.maps.find(MapKey(id, config_.platform));
      if (found == index_.maps.end()) continue;
      CachedMap& cached = found->second;
      const std::string version_dir = Join(config_.dir, cached.dir);
      // The package index first: its new stamp is what the next mount reads it by.
      std::string unequal;
      std::string sha;
      FileStamp stamp;
      if (!HashFile(Join(version_dir, kPackageFile), &sha, &stamp) || sha != cached.index_sha256) {
        unequal = "index_hash";
      } else {
        cached.index_stamp = stamp;
      }
      for (CachedFile& f : cached.files) {
        if (!unequal.empty()) break;
        if (!HashFile(Join(version_dir, f.path), &sha, &stamp) || sha != f.sha256) {
          unequal = "member_hash";
          break;
        }
        f.stamp = stamp;
      }
      if (!keep_going_()) return;
      dirty_ = true;
      if (!unequal.empty()) {
        env_.Event(EventLevel::kWarn, "map_rehash", {{"id", id}, {"result", "dropped"}, {"reason", unequal}});
        index_.maps.erase(found);
        continue;
      }
      if (!cached.stock_stamps.empty() &&
          (deriver_ == nullptr || !deriver_->StockCurrent(UniqueByPath(cached.stock_stamps)))) {
        Package package;
        std::string reason = deriver_ == nullptr ? "derived_unsupported" : (ReadPackage(cached, &package) ? "" : "package");
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

  // Rebuilds over a version directory nothing holds (the mount deferred the map): the rebuilt files and their stamps replace the
  // old ones in `cached`.
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

  // Runs the deriver over `version_dir` and appends the merged set and the rebuilt scripts to `files`, frames checked. Stamps are
  // left for the caller, which takes them where the files finally live.
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
      if (!SizeOf(Join(version_dir, merged.path), &merged.size)) return "derived_output";
      merged.frames_checked = CheckFrames(Join(version_dir, merged.path), merged.path);
      files->push_back(std::move(merged));
    }
    for (const RekeyPin& r : package.rekeyed) {
      if (r.edits.empty()) continue;
      const auto script = std::find_if(out.scripts.begin(), out.scripts.end(),
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

  // Every frame header of a set (core/map_set_tables.h), read after its pieces matched their pins. A problem is logged and
  // recorded as frames_checked=false; the registry's own check at the mount decides.
  bool CheckFrames(const std::string& path, const std::string& member_path) {
    const SetProblem problem = CheckSetFile(env_, path, table_bytes_);
    if (problem.reason.empty()) return true;
    env_.Event(EventLevel::kWarn, "map_member",
               {{"path", member_path}, {"frames", problem.reason}, {"index", problem.index},
                {"consequence", "the set is mounted only if the registry's own check passes"}});
    return false;
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

  // A zero stamp when the file system has no file id: the mount then defers with stamp_unsupported.
  bool Stamp(const std::string& path, FileStamp* stamp) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!fs_.OpenHeld(path, &handle, &size)) return false;
    if (!fs_.StampOf(handle, stamp)) *stamp = FileStamp();
    env_.CloseBlob(handle);
    return true;
  }

  // GETs `url` into memory, at most `cap` bytes. Returns a reason or "".
  std::string GetSmall(const std::string& url, std::uint64_t cap, std::string* out) {
    out->clear();
    long status = 0;
    bool too_large = false;
    const HttpResult http = env_.Get(
        url,
        [&](long st, const void* data, std::size_t length) {
          status = st;
          if (!IsWholeBody(st)) return false;
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
    if (!IsWholeBody(final_status)) return HttpReason(final_status);
    if (!http.completed) return "transport";
    return std::string();
  }

  // The package index blob, checked against its pin, with the piece retry rule. It is hashed once, here.
  std::string FetchIndex(const Pin& pin, std::string* out) {
    if (pin.size == 0 || pin.size > std::min(kMaxIndexBytes, document_bytes_)) return "over_cap";
    std::string reason;
    for (int attempt = 0; attempt < kDownloadAttempts; ++attempt) {
      reason = GetSmall(config_.base_url + kBlobUrlPrefix + pin.sha256, pin.size, out);
      if (reason == "cancelled" || reason == "over_cap") return reason;
      if (!reason.empty()) continue;
      std::unique_ptr<nevr_cargo::Hasher> hasher = env_.NewHasher();
      hasher->Update(out->data(), out->size());
      if (out->size() == pin.size && hasher->Hex() == pin.sha256) return std::string();
      reason = "index_hash";
    }
    return reason;
  }

  // A piece a cached file already holds, copied into `file` without hashing it again: the file's stamp must still be the one it
  // had when its bytes were verified. False (with `file` cut back) when the piece cannot be had that way: fetch it.
  bool ReusePiece(const Pin& piece, nevr_cargo::AppendFile& file) {
    const auto found = catalog_.find(piece.sha256);
    if (found == catalog_.end() || found->second.size != piece.size) return false;
    const PieceLocation where = found->second;
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!fs_.OpenHeld(where.path, &handle, &size)) {
      catalog_.erase(found);
      return false;
    }
    FileStamp stamp;
    if (where.stamped && (!fs_.StampOf(handle, &stamp) || stamp != where.stamp)) {
      env_.CloseBlob(handle);
      catalog_.erase(found);
      return false;
    }
    const std::uint64_t start = file.Size();
    bool ok = Inside(where.offset, where.size, size);
    for (std::uint64_t done = 0; ok && done < where.size;) {
      const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer_.size(), where.size - done));
      ok = ReadExact(env_, handle, where.offset + done, buffer_.data(), length) && file.Append(buffer_.data(), length);
      done += length;
    }
    env_.CloseBlob(handle);
    if (!ok) {
      file.Truncate(start);
      catalog_.erase(piece.sha256);
    }
    return ok;
  }

  // One GET of a piece, appended to `file` while it is hashed. Any failure cuts the file back to where the piece started.
  Download GetPiece(const Pin& piece, nevr_cargo::AppendFile& file) {
    Download out;
    const std::uint64_t start = file.Size();
    std::unique_ptr<nevr_cargo::Hasher> hasher = env_.NewHasher();
    std::uint64_t got = 0;
    long status = 0;
    bool too_large = false, write_failed = false;
    const HttpResult http = env_.Get(
        config_.base_url + kBlobUrlPrefix + piece.sha256,
        [&](long st, const void* data, std::size_t length) {
          status = st;
          if (!IsWholeBody(st)) return false;
          if (got + length > piece.size) {
            too_large = true;
            return false;
          }
          hasher->Update(data, length);
          if (!file.Append(data, length)) {
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
    } else if (!IsWholeBody(final_status)) {
      out.reason = HttpReason(final_status);
      out.retry = true;
    } else if (!http.completed) {
      out.reason = "transport";
      out.retry = true;
    } else if (got != piece.size || hasher->Hex() != piece.sha256) {
      out.reason = "piece_hash";
      out.retry = true;
    }
    if (!out.reason.empty()) {
      file.Truncate(start);
      return out;
    }
    report_.bytes_fetched += got;
    return out;
  }

  // The resume points a stopped transfer of this version left, each still valid only while its file's stamp is unchanged.
  std::map<std::string, ResumePoint> ReadResume(const std::string& staging) {
    std::map<std::string, ResumePoint> points;
    std::string text;
    if (!env_.ReadSmallFile(Join(Join(staging, kResumeDir), kResumeFile), &text,
                            static_cast<std::size_t>(document_bytes_))) {
      return points;
    }
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) return points;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
      const json& v = it.value();
      ResumePoint p;
      if (!v.is_object() || !v.contains("pieces") || !v.contains("bytes") || !v.contains("stamp")) continue;
      const json& s = v["stamp"];
      if (!v["pieces"].is_number_unsigned() || !v["bytes"].is_number_unsigned() || !s.is_array() ||
          s.size() != kStampFields) {
        continue;
      }
      bool numbers = true;
      for (const json& n : s) numbers = numbers && n.is_number_unsigned();
      if (!numbers) continue;
      p.pieces = v["pieces"].get<std::uint64_t>();
      p.bytes = v["bytes"].get<std::uint64_t>();
      std::uint64_t* const fields[kStampFields] = {&p.stamp.volume, &p.stamp.id_hi, &p.stamp.id_lo, &p.stamp.size,
                                                    &p.stamp.last_write};
      for (std::size_t i = 0; i < kStampFields; ++i) *fields[i] = s[i].get<std::uint64_t>();
      points[it.key()] = p;
    }
    return points;
  }

  // Records how far each member came, with the stamp its file has now, so the next fetch resumes after the verified pieces.
  void WriteResume(const std::string& staging, const std::map<std::string, ResumePoint>& reached) {
    json doc = json::object();
    for (const auto& [path, point] : reached) {
      FileStamp stamp;
      if (!Stamp(Join(staging, path), &stamp) || stamp == FileStamp()) continue;  // no file id: nothing to hold the bytes to
      doc[path] = json{{"pieces", point.pieces},
                       {"bytes", point.bytes},
                       {"stamp", json::array({stamp.volume, stamp.id_hi, stamp.id_lo, stamp.size, stamp.last_write})}};
    }
    const std::string dir = Join(staging, kResumeDir);
    if (!env_.MakeDirs(dir) || !env_.WriteFileAtomically(Join(dir, kResumeFile), doc.dump())) {
      env_.Event(EventLevel::kWarn, "map_member",
                 {{"path", staging}, {"reason", "resume_write"}, {"consequence", "the next fetch starts this map again"}});
    }
  }

  // Appends the member's pieces, each copied from the cache or fetched, to staging/<member path>, after the pieces a stopped
  // transfer verified. Returns a reason or "", and how far it came in `reached`.
  std::string AssembleMember(const std::string& staging, const Member& member, const std::map<std::string, ResumePoint>& resume,
                             std::map<std::string, ResumePoint>* reached, std::size_t* fetched, std::size_t* reused) {
    const std::string path = Join(staging, member.path);
    std::uint64_t first = 0, bytes = 0;
    const auto point = resume.find(member.path);
    if (point != resume.end() && point->second.pieces <= member.pieces.size()) {
      FileStamp stamp;
      if (Stamp(path, &stamp) && stamp == point->second.stamp && stamp.size >= point->second.bytes) {
        first = point->second.pieces;
        bytes = point->second.bytes;
      }
    }
    std::unique_ptr<nevr_cargo::AppendFile> file = env_.OpenAppend(path);
    if (!file || !file->Truncate(bytes)) return "write_failed";
    if (first > 0) {
      *reused += static_cast<std::size_t>(first);
      report_.pieces_reused += static_cast<std::size_t>(first);
    }
    for (std::size_t i = static_cast<std::size_t>(first); i < member.pieces.size(); ++i) {
      const Pin& piece = member.pieces[i];
      (*reached)[member.path] = ResumePoint{i, file->Size(), FileStamp()};
      if (!keep_going_()) return "cancelled";
      if (ReusePiece(piece, *file)) {
        ++*reused;
        ++report_.pieces_reused;
        Progress();
        continue;
      }
      Download download;
      for (int attempt = 0; attempt < kDownloadAttempts; ++attempt) {
        download = GetPiece(piece, *file);
        if (!download.retry) break;
      }
      if (!download.reason.empty()) {
        env_.Event(download.reason == "cancelled" ? EventLevel::kInfo : EventLevel::kWarn, "map_piece",
                   {{"sha256", piece.sha256}, {"reason", download.reason}});
        if (!file->Sync()) return "write_failed";
        return download.reason;
      }
      ++*fetched;
      ++report_.pieces_fetched;
      Progress();
    }
    if (!file->Sync()) return "write_failed";
    if (file->Size() != member.size) return "member_size";
    (*reached)[member.path] = ResumePoint{member.pieces.size(), member.size, FileStamp()};
    return std::string();
  }

  void PlanAndFetch() {
    const Manifest& manifest = parsed_.manifest;
    std::uint64_t planned = 0;
    struct Planned {
      std::string id;
      const PlatformEntry* entry = nullptr;
    };
    std::vector<Planned> to_fetch;
    for (const auto& [id, map] : manifest.maps) {
      const auto platform = map.platforms.find(config_.platform);
      if (platform == map.platforms.end()) continue;
      const PlatformEntry& entry = platform->second;
      const std::string key = MapKey(id, config_.platform);
      auto plan = [&](const char* reason, EventLevel level = EventLevel::kInfo) {
        env_.Event(level, "map_plan", {{"group", group_}, {"id", id}, {"reason", reason}});
      };
      report_.set_bytes += entry.bytes;
      const LocalPrecedence local = DecideLocalPrecedence({id, entry.level_name}, local_names_);
      if (local.skip) {
        plan(kLocalMapToken);
        continue;
      }
      if (Contains(manifest.revoked_ids, id) || revoked_sha_.count(entry.index.sha256)) {
        Drop(key);
        plan("revoked", EventLevel::kWarn);
        continue;
      }
      if (!GameBuildOk(config_, entry.game_build)) {
        plan("unsupported_game", EventLevel::kWarn);
        continue;
      }
      if (entry.scripts > 0 && config_.signed_only && !parsed_.official) {
        plan("signed_only", EventLevel::kWarn);
        continue;
      }
      const auto cached = index_.maps.find(key);
      if (cached != index_.maps.end() && cached->second.index_sha256 == entry.index.sha256) {
        plan("current");
        continue;
      }
      // What this pass adds to the disk stays within the device's storage share (ADR 0045 decision 8).
      if (entry.bytes > limits_.storage_budget - std::min(planned, limits_.storage_budget)) {
        plan("over_budget", EventLevel::kWarn);
        MapFailed("over_budget");
        continue;
      }
      planned += entry.bytes;
      plan("fetch");
      to_fetch.push_back(Planned{id, &entry});
    }
    report_.bytes_to_fetch = planned;
    Progress();
    for (const Planned& p : to_fetch) {
      if (!keep_going_()) return;
      FetchMap(p.id, *p.entry);
    }
  }

  void MapFailed(const std::string& reason) {
    if (first_map_failure_.empty()) first_map_failure_ = reason;
  }

  void FetchMap(const std::string& id, const PlatformEntry& entry) {
    const std::string key = MapKey(id, config_.platform);
    auto fail = [&](const char* event, const std::string& why, bool failure = true) {
      env_.Event(why == "cancelled" ? EventLevel::kInfo : EventLevel::kWarn, event,
                 {{"id", id}, {"version", entry.version}, {"reason", why}});
      if (failure) MapFailed(why);
    };
    std::string index_bytes;
    std::string reason = FetchIndex(entry.index, &index_bytes);
    if (!reason.empty()) return fail("map_index", reason);
    const ParsedPackage parsed = ParsePackageIndex(index_bytes, id, config_.platform, entry, document_bytes_);
    if (!parsed.reason.empty()) return fail("map_index", parsed.reason);
    const Package& package = parsed.package;
    // The plan charged entry.bytes against the storage share, so the members may not add up to more.
    std::uint64_t member_bytes = 0;
    for (const Member& m : package.members) member_bytes += m.size;
    if (member_bytes > entry.bytes) return fail("map_index", "bytes_mismatch");
    if (!RevokedSha(revoked_sha_, entry.index.sha256, package).empty()) {
      Drop(key);
      return fail("map_plan", "revoked", false);
    }
    if (!StockManifestOk(package, config_)) return fail("map_plan", "stock_mismatch", false);
    const bool derive = NeedsDerive(package);
    if (derive && deriver_ == nullptr) return fail("map_derive", "derived_unsupported", false);

    const std::string rel = id + "/" + config_.platform + "/" + VersionDirName(entry.index.sha256);
    const std::string final_dir = Join(config_.dir, rel);
    const std::string staging = final_dir + kStagingSuffix;
    std::map<std::string, ResumePoint> reached;
    auto abandon = [&](const char* event, const std::string& why) {
      // A stop or a transport failure keeps the verified pieces for the next fetch to resume from; anything else starts over.
      if (Resumable(why)) {
        WriteResume(staging, reached);
      } else {
        fs_.RemoveTree(staging);
      }
      fail(event, why);
    };
    if (!env_.MakeDirs(staging)) return fail("map_ready", "mkdir");
    const std::map<std::string, ResumePoint> resume = ReadResume(staging);
    if (!fs_.RemoveTree(Join(staging, kResumeDir))) return fail("map_ready", "staging_busy");
    for (const Member& m : package.members) {
      if (!env_.MakeDirs(ParentOf(Join(staging, m.path)))) return abandon("map_ready", "mkdir");
    }
    if (!env_.WriteFileAtomically(Join(staging, kPackageFile), index_bytes)) return abandon("map_ready", "write_failed");

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
      reason = AssembleMember(staging, m, resume, &reached, &fetched, &reused);
      if (!reason.empty()) return abandon("map_member", reason);
      if (m.role == Role::kRecipe) continue;
      CachedFile file;
      file.path = m.path;
      file.role = m.role == Role::kSet ? FileRole::kSet : FileRole::kScript;
      file.script_hash = m.script_hash;
      file.size = m.size;
      file.sha256 = m.sha256;
      if (m.role == Role::kSet) file.frames_checked = CheckFrames(Join(staging, m.path), m.path);
      cached.files.push_back(std::move(file));
    }
    if (derive) {
      reason = RunDerive(staging, package, &cached.files, &cached.stock_stamps);
      if (!reason.empty()) return abandon("map_derive", reason);
    }
    if (!fs_.RemoveTree(final_dir)) return abandon("map_ready", "dir_busy");
    if (!fs_.RenameDir(staging, final_dir)) return abandon("map_ready", "rename");
    for (CachedFile& f : cached.files) {
      if (!Stamp(Join(final_dir, f.path), &f.stamp)) return fail("map_ready", "stamp");
    }
    if (!Stamp(Join(final_dir, kPackageFile), &cached.index_stamp)) return fail("map_ready", "stamp");
    AddToCatalog(final_dir, package, cached.files);
    index_.maps[key] = std::move(cached);
    dirty_ = true;
    ++report_.maps_updated;
    env_.Event(EventLevel::kInfo, "map_ready",
               {{"group", group_}, {"id", id}, {"version", entry.version}, {"pieces_fetched", fetched},
                {"pieces_reused", reused}});
  }

  static constexpr std::size_t kStampFields = 5;  // volume, id_hi, id_lo, size, last_write

  Env& env_;
  MapFs& fs_;
  const ChannelConfig& config_;
  const std::string group_;
  Deriver* deriver_;
  const std::function<bool()>& keep_going_;
  const FetchProgressFn& progress_fn_;
  nevr_cargo::Limits limits_;
  std::uint64_t document_bytes_ = 0;
  std::uint64_t table_bytes_ = 0;
  std::uint64_t started_ = 0;
  std::vector<char> buffer_;
  CacheIndex index_;
  ParsedManifest parsed_;
  std::set<std::string> revoked_sha_;
  std::set<std::string> local_names_;  // this run's local level names and local map ids (core/map_local_precedence.h)
  std::unordered_map<std::string, PieceLocation> catalog_;
  std::string first_map_failure_;
  bool dirty_ = false;
  FetchReport report_;
};

}  // namespace

std::string GroupManifestUrl(const std::string& base_url, const std::string& group) {
  return base_url + kGroupManifestUrlPrefix + group + kGroupManifestUrlSuffix;
}

std::string StoredGroupManifestPath(const std::string& dir, const std::string& group) {
  return Join(Join(Join(dir, kGroupManifestsDir), group), kStoredManifestFile);
}

std::vector<std::string> CachedGroupIds(Env& env, const std::string& dir) {
  CacheIndex index;
  std::vector<std::string> ids;
  if (ReadCacheIndex(env, dir, &index) != CacheIndexStatus::kOk) return ids;
  for (const CachedGroup& g : index.groups) ids.push_back(g.id);
  return ids;
}

const char* FetchOutcomeName(FetchOutcome outcome) noexcept {
  switch (outcome) {
    case FetchOutcome::kFetched:
      return "fetched";
    case FetchOutcome::kCached:
      return "cached";
    case FetchOutcome::kFailed:
      return "failed";
  }
  return "failed";
}

CacheMount::CacheMount(Env& env, MapFs& fs, const ChannelConfig& config, Mounter& mounter, Deriver* deriver)
    : env_(env), fs_(fs), config_(config), mounter_(mounter), deriver_(deriver) {}

MountReport CacheMount::Mount(const std::vector<std::string>& groups, const std::string& now_utc) {
  return MountRun(env_, fs_, config_, mounter_, deriver_, mounted_, script_holders_).Run(groups, now_utc);
}

FetchReport FetchGroup(Env& env, MapFs& fs, const ChannelConfig& config, const std::string& group,
                       const std::vector<std::string>& deferred, Deriver* deriver, const std::string& now_utc,
                       const std::function<bool()>& keep_going, const FetchProgressFn& progress) {
  return FetchRun(env, fs, config, group, deriver, keep_going, progress).Run(deferred, now_utc);
}

}  // namespace nevr_maps
