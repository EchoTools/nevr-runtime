#include "core/map_index.h"

#include <algorithm>
#include <limits>

#include <nlohmann/json.hpp>

#include "core/cargo_format.h"

namespace nevr_maps {
namespace {

using json = nlohmann::json;

using nevr_cargo::FormatHex64;

bool ParseHex64(const json& value, std::uint64_t* out) {
  return value.is_string() && nevr_cargo::ParseHex64(value.get_ref<const std::string&>(), out);
}

bool GetString(const json& object, const char* key, std::string* out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) return false;
  *out = it->get<std::string>();
  return true;
}

bool GetUnsigned(const json& object, const char* key, std::uint64_t* out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number_unsigned()) return false;
  *out = it->get<std::uint64_t>();
  return true;
}

bool GetBool(const json& object, const char* key, bool* out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_boolean()) return false;
  *out = it->get<bool>();
  return true;
}

const char* RoleName(FileRole role) {
  switch (role) {
    case FileRole::kSet:
      return "set";
    case FileRole::kScript:
      return "script";
    case FileRole::kMergedSet:
      return "merged_set";
    case FileRole::kDerivedScript:
      return "derived_script";
  }
  return "set";
}

bool ParseRole(const std::string& text, FileRole* out) {
  for (FileRole role : {FileRole::kSet, FileRole::kScript, FileRole::kMergedSet, FileRole::kDerivedScript}) {
    if (text == RoleName(role)) {
      *out = role;
      return true;
    }
  }
  return false;
}

json StampToJson(const FileStamp& s) {
  return json{
      {"volume", s.volume}, {"id_hi", s.id_hi}, {"id_lo", s.id_lo}, {"size", s.size}, {"last_write", s.last_write}};
}

bool StampFromJson(const json& value, FileStamp* out) {
  return value.is_object() && GetUnsigned(value, "volume", &out->volume) && GetUnsigned(value, "id_hi", &out->id_hi) &&
         GetUnsigned(value, "id_lo", &out->id_lo) && GetUnsigned(value, "size", &out->size) &&
         GetUnsigned(value, "last_write", &out->last_write);
}

bool IsScriptRole(FileRole role) { return role == FileRole::kScript || role == FileRole::kDerivedScript; }

json MapToJson(const CachedMap& m) {
  json files = json::array();
  for (const CachedFile& f : m.files) {
    json file{{"path", f.path},     {"role", RoleName(f.role)},      {"size", f.size},
              {"sha256", f.sha256}, {"stamp", StampToJson(f.stamp)}, {"frames_checked", f.frames_checked}};
    if (IsScriptRole(f.role)) file["hash"] = FormatHex64(f.script_hash);
    files.push_back(std::move(file));
  }
  json stock = json::array();
  for (const StockStamp& s : m.stock_stamps) stock.push_back(json{{"path", s.path}, {"stamp", StampToJson(s.stamp)}});
  return json{{"id", m.id},
              {"platform", m.platform},
              {"version", m.version},
              {"index_sha256", m.index_sha256},
              {"index_size", m.index_size},
              {"level", FormatHex64(m.level)},
              {"level_name", m.level_name},
              {"game_build", m.game_build},
              {"dir", m.dir},
              {"pinned_serial", m.pinned_serial},
              {"stock_manifest_ok", m.stock_manifest_ok},
              {"files", std::move(files)},
              {"stock_stamps", std::move(stock)}};
}

bool MapFromJson(const json& value, CachedMap* out) {
  if (!value.is_object()) return false;
  const auto level = value.find("level");
  if (!GetString(value, "id", &out->id) || !GetString(value, "platform", &out->platform) ||
      !GetString(value, "version", &out->version) || !GetString(value, "index_sha256", &out->index_sha256) ||
      !GetUnsigned(value, "index_size", &out->index_size) || level == value.end() || !ParseHex64(*level, &out->level) ||
      !GetString(value, "level_name", &out->level_name) || !GetString(value, "game_build", &out->game_build) ||
      !GetString(value, "dir", &out->dir) || !GetUnsigned(value, "pinned_serial", &out->pinned_serial) ||
      !GetBool(value, "stock_manifest_ok", &out->stock_manifest_ok)) {
    return false;
  }
  const auto files = value.find("files");
  const auto stock = value.find("stock_stamps");
  if (files == value.end() || !files->is_array() || stock == value.end() || !stock->is_array()) return false;
  for (const json& f : *files) {
    CachedFile file;
    std::string role;
    if (!f.is_object() || !GetString(f, "path", &file.path) || !GetString(f, "role", &role) ||
        !ParseRole(role, &file.role) || !GetUnsigned(f, "size", &file.size) || !GetString(f, "sha256", &file.sha256) ||
        !GetBool(f, "frames_checked", &file.frames_checked)) {
      return false;
    }
    const auto stamp = f.find("stamp");
    if (stamp == f.end() || !StampFromJson(*stamp, &file.stamp)) return false;
    if (IsScriptRole(file.role)) {
      const auto hash = f.find("hash");
      if (hash == f.end() || !ParseHex64(*hash, &file.script_hash)) return false;
    }
    out->files.push_back(std::move(file));
  }
  for (const json& s : *stock) {
    StockStamp stamp;
    const auto inner = s.is_object() ? s.find("stamp") : s.end();
    if (!s.is_object() || !GetString(s, "path", &stamp.path) || inner == s.end() ||
        !StampFromJson(*inner, &stamp.stamp)) {
      return false;
    }
    out->stock_stamps.push_back(std::move(stamp));
  }
  return true;
}

bool IsKeyIdText(const std::string& text) {
  return text.size() == nevr_cargo::kHexCharsPerByte * kMinisignKeyIdBytes &&
         std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); });
}

json FloorsToJson(const FloorBook& book) {
  json channels = json::object();
  for (const auto& [name, by_guild] : book.channels) {
    json guilds = json::object();
    for (const auto& [guild, by_key] : by_guild) {
      json keys = json::object();
      for (const auto& [key_id, floors] : by_key) {
        json versions = json::object();
        for (const auto& [key, version] : floors.version) versions[key] = version;
        keys[key_id] =
            json{{"serial", floors.serial}, {"serial_sha256", floors.serial_sha256}, {"version", std::move(versions)}};
      }
      guilds[guild] = std::move(keys);
    }
    channels[name] = std::move(guilds);
  }
  json bound = json::object();
  for (const auto& [url, channel] : book.bound) bound[url] = channel;
  return json{{"channels", std::move(channels)}, {"bound", std::move(bound)}};
}

bool FloorsFromJson(const json& value, FloorBook* out) {
  if (!value.is_object()) return false;
  const auto channels = value.find("channels");
  const auto bound = value.find("bound");
  if (channels == value.end() || !channels->is_object() || bound == value.end() || !bound->is_object()) return false;
  for (auto it = channels->begin(); it != channels->end(); ++it) {
    if (!it.value().is_object()) return false;
    auto& by_guild = out->channels[it.key()];
    for (auto g = it.value().begin(); g != it.value().end(); ++g) {
      // Floors are kept per guild (ADR 0047 decision 3): an entry that is not a guild id is not a floor and is dropped; the
      // next accepted manifest of each guild sets its own.
      if (!IsPathSafeGroupId(g.key()) || !g.value().is_object()) continue;
      auto& by_key = by_guild[g.key()];
      for (auto k = g.value().begin(); k != g.value().end(); ++k) {
        // A floor whose name is not a key id has no key to hold it to, so it is dropped the same way.
        if (!IsKeyIdText(k.key())) continue;
        Floors floors;
        const json& c = k.value();
        if (!c.is_object() || !GetUnsigned(c, "serial", &floors.serial) ||
            !GetString(c, "serial_sha256", &floors.serial_sha256)) {
          return false;
        }
        const auto versions = c.find("version");
        if (versions == c.end() || !versions->is_object()) return false;
        for (auto v = versions->begin(); v != versions->end(); ++v) {
          if (!v.value().is_string()) return false;
          floors.version[v.key()] = v.value().get<std::string>();
        }
        by_key[k.key()] = std::move(floors);
      }
      if (by_key.empty()) by_guild.erase(g.key());
    }
    if (by_guild.empty()) out->channels.erase(it.key());
  }
  for (auto it = bound->begin(); it != bound->end(); ++it) {
    if (!it.value().is_string()) return false;
    out->bound[it.key()] = it.value().get<std::string>();
  }
  return true;
}

json GroupsToJson(const std::vector<CachedGroup>& groups) {
  json out = json::array();
  for (const CachedGroup& g : groups) out.push_back(json{{"id", g.id}, {"bytes", g.bytes}, {"maps", g.maps}});
  return out;
}

// An index written before groups were kept has none: its maps are kept until a group's fetch lists them again.
bool GroupsFromJson(const json& doc, std::vector<CachedGroup>* out) {
  const auto groups = doc.find("groups");
  if (groups == doc.end()) return true;
  if (!groups->is_array()) return false;
  for (const json& g : *groups) {
    CachedGroup group;
    const auto maps = g.is_object() ? g.find("maps") : g.end();
    if (!g.is_object() || !GetString(g, "id", &group.id) || !IsPathSafeGroupId(group.id) ||
        !GetUnsigned(g, "bytes", &group.bytes) || maps == g.end() || !maps->is_array()) {
      return false;
    }
    for (const json& key : *maps) {
      if (!key.is_string()) return false;
      group.maps.push_back(key.get<std::string>());
    }
    const bool repeated = std::any_of(out->begin(), out->end(), [&](const CachedGroup& o) { return o.id == group.id; });
    if (repeated) return false;
    out->push_back(std::move(group));
  }
  return true;
}

}  // namespace

bool CachedMap::operator==(const CachedMap& o) const {
  if (stock_stamps.size() != o.stock_stamps.size()) return false;
  for (std::size_t i = 0; i < stock_stamps.size(); ++i) {
    if (stock_stamps[i].path != o.stock_stamps[i].path || stock_stamps[i].stamp != o.stock_stamps[i].stamp)
      return false;
  }
  return id == o.id && platform == o.platform && version == o.version && index_sha256 == o.index_sha256 &&
         level_name == o.level_name && game_build == o.game_build && index_size == o.index_size && level == o.level &&
         dir == o.dir && pinned_serial == o.pinned_serial && stock_manifest_ok == o.stock_manifest_ok &&
         files == o.files;
}

std::string MapKey(const std::string& id, const std::string& platform) { return id + "/" + platform; }

std::string VersionDirName(const std::string& index_sha256) { return index_sha256.substr(0, kVersionDirChars); }

std::string CacheIndexToJson(const CacheIndex& index) {
  json maps = json::object();
  for (const auto& [key, m] : index.maps) maps[key] = MapToJson(m);
  const json doc{{"schema", kCacheIndexSchema},
                 {"schema_version", kCacheIndexSchemaVersion},
                 {"floors", FloorsToJson(index.floors)},
                 {"maps", std::move(maps)},
                 {"groups", GroupsToJson(index.groups)}};
  return doc.dump();
}

bool CacheIndexFromJson(std::string_view text, CacheIndex* out) {
  *out = CacheIndex();
  const json doc = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
  std::string schema;
  std::uint64_t version = 0;
  if (doc.is_discarded() || !doc.is_object() || !GetString(doc, "schema", &schema) || schema != kCacheIndexSchema ||
      !GetUnsigned(doc, "schema_version", &version) || version != kCacheIndexSchemaVersion) {
    return false;
  }
  CacheIndex index;
  const auto floors = doc.find("floors");
  const auto maps = doc.find("maps");
  if (floors == doc.end() || !FloorsFromJson(*floors, &index.floors) || maps == doc.end() || !maps->is_object())
    return false;
  for (auto it = maps->begin(); it != maps->end(); ++it) {
    CachedMap m;
    if (!MapFromJson(it.value(), &m) || it.key() != MapKey(m.id, m.platform)) return false;
    index.maps[it.key()] = std::move(m);
  }
  if (!GroupsFromJson(doc, &index.groups)) return false;
  *out = std::move(index);
  return true;
}

const char* CacheIndexStatusName(CacheIndexStatus status) noexcept {
  switch (status) {
    case CacheIndexStatus::kOk:
      return "ok";
    case CacheIndexStatus::kAbsent:
      return "absent";
    case CacheIndexStatus::kTooLarge:
      return "too_large";
    case CacheIndexStatus::kInvalid:
      return "invalid";
    case CacheIndexStatus::kNoMemoryFact:
      return "no_memory_fact";
  }
  return "invalid";
}

CacheIndexStatus ReadCacheIndex(nevr_cargo::Env& env, const std::string& dir, CacheIndex* out) {
  *out = CacheIndex();
  nevr_cargo::DeviceFacts facts;
  const std::uint64_t max_bytes = env.QueryDevice(dir, &facts) ? MapDocumentBytes(facts) : 0;
  if (max_bytes == 0) return CacheIndexStatus::kNoMemoryFact;
  const std::string path = dir + "/" + kCacheIndexFile;
  std::string text;
  if (!env.ReadSmallFile(path, &text, static_cast<std::size_t>(std::min<std::uint64_t>(max_bytes, std::numeric_limits<std::size_t>::max())))) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!env.OpenBlob(path, &handle, &size)) return CacheIndexStatus::kAbsent;
    env.CloseBlob(handle);
    return size > max_bytes ? CacheIndexStatus::kTooLarge : CacheIndexStatus::kAbsent;
  }
  if (!CacheIndexFromJson(text, out)) {
    env.WriteFileAtomically(path + ".bad", text);
    return CacheIndexStatus::kInvalid;
  }
  return CacheIndexStatus::kOk;
}

bool WriteCacheIndex(nevr_cargo::Env& env, const std::string& dir, const CacheIndex& index) {
  return env.WriteFileAtomically(dir + "/" + kCacheIndexFile, CacheIndexToJson(index));
}

}  // namespace nevr_maps
