#include "runtime/patch/resource_set_registry.h"

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "nevr/resource_set.h"

namespace nevr_resource_sets {

namespace {

using Key = std::pair<std::uint64_t, std::uint64_t>;

template <typename T>
T LoadAt(const unsigned char* base, std::size_t at) {
  T v{};
  std::memcpy(&v, base + at, sizeof(v));
  return v;
}

bool SortedHas(const std::vector<Key>& sorted, const Key& key) {
  return std::binary_search(sorted.begin(), sorted.end(), key);
}

std::vector<Key> SortedKeys(const Set& set) {
  std::vector<Key> keys;
  keys.reserve(set.records.size());
  for (const nevr_shadow::Record& r : set.records) keys.emplace_back(r.type, r.name);
  std::sort(keys.begin(), keys.end());
  return keys;
}

Outcome Refuse(Refusal refusal, std::string detail) { return Outcome{refusal, std::move(detail)}; }

// The archive of `levelName` in `view`: decompressed from its chunk and parsed. The parse has already checked that
// the record lies inside its chunk's decompressed bytes and that the frame lies inside the set.
Outcome ReadLevel(const nevr_resource_set::View& view, const nevr_resource_set::Record& archive, Level* out) {
  const nevr_resource_set::Chunk chunk = view.ChunkAt(archive.chunk);
  std::vector<unsigned char> plain(chunk.decompressed);
  const std::size_t got = ZSTD_decompress(plain.data(), plain.size(), view.data() + chunk.offset, chunk.compressed);
  const std::string key = KeyText(archive.type, archive.name);
  if (ZSTD_isError(got) != 0 || got != plain.size()) {
    return Refuse(Refusal::kArchive, key + " (its chunk does not decompress to " + std::to_string(chunk.decompressed) +
                                         " bytes)");
  }
  out->name = archive.name;
  if (!ParseArchive(plain.data() + archive.offset_in_chunk, archive.size, out)) {
    return Refuse(Refusal::kArchive, key + " (its body does not parse as an archive)");
  }
  return Outcome{};
}

}  // namespace

const char* RefusalName(Refusal refusal) {
  switch (refusal) {
    case Refusal::kNone: return "none";
    case Refusal::kFormat: return "format";
    case Refusal::kScript: return "script_resource";
    case Refusal::kTexturePayload: return "texture_without_payload";
    case Refusal::kLevelIncomplete: return "level_without_archive";
    case Refusal::kArchive: return "archive_unreadable";
    case Refusal::kCollides: return "collides";
    case Refusal::kTooLate: return "too_late";
    case Refusal::kStockKey: return "stock_key";
    case Refusal::kUnresolved: return "unresolved";
  }
  return "unknown";
}

std::string KeyText(std::uint64_t type, std::uint64_t name) {
  char text[48];
  std::snprintf(text, sizeof(text), "0x%016llx:0x%016llx", static_cast<unsigned long long>(type),
                static_cast<unsigned long long>(name));
  return text;
}

bool ParseArchive(const unsigned char* data, std::size_t size, Level* out) {
  out->dependencies.clear();
  out->entries.clear();
  std::size_t at = 0;
  if (size < 4) return false;
  const std::uint32_t deps = LoadAt<std::uint32_t>(data, at);
  at += 4;
  if (deps > (size - at) / 8) return false;
  for (std::uint32_t i = 0; i < deps; ++i, at += 8) out->dependencies.push_back(LoadAt<std::uint64_t>(data, at));
  if (size - at < 4) return false;
  const std::uint32_t entries = LoadAt<std::uint32_t>(data, at);
  at += 4;
  if (entries > (size - at) / 16) return false;
  for (std::uint32_t i = 0; i < entries; ++i, at += 16) {
    out->entries.emplace_back(LoadAt<std::uint64_t>(data, at), LoadAt<std::uint64_t>(data, at + 8));
  }
  return true;
}

Outcome Registry::Add(const std::string& plugin, const std::string& label, const void* bytes, std::uint64_t size,
                      std::int64_t handle, std::uint64_t fileOffset) {
  if (mounted_) return Refuse(Refusal::kTooLate, "the run's resource sets are already mounted");
  nevr_resource_set::View view;
  const nevr_resource_set::Result parsed = nevr_resource_set::Parse(bytes, size, &view);
  if (!parsed.ok()) {
    return Refuse(Refusal::kFormat, std::string(nevr_resource_set::ProblemName(parsed.problem)) + " at index " +
                                        std::to_string(parsed.index));
  }

  std::vector<Key> keys;
  keys.reserve(view.record_count());
  for (std::uint32_t i = 0; i < view.record_count(); ++i) {
    const nevr_resource_set::Record r = view.RecordAt(i);
    keys.emplace_back(r.type, r.name);
  }
  std::vector<Key> sorted(keys);
  std::sort(sorted.begin(), sorted.end());

  auto set = std::make_unique<Set>();
  set->plugin = plugin;
  set->label = label;
  set->handle = handle;
  set->bytes = size;
  for (std::uint32_t i = 0; i < view.record_count(); ++i) {
    const nevr_resource_set::Record r = view.RecordAt(i);
    if (r.type == kScriptType) return Refuse(Refusal::kScript, KeyText(r.type, r.name));
    if (r.type == kTextureType && !SortedHas(sorted, Key{kTextureGpuType, r.name})) {
      return Refuse(Refusal::kTexturePayload, KeyText(r.type, r.name));
    }
    if (r.type != kLevelRootType) continue;
    std::uint32_t archiveIndex = view.record_count();
    for (std::uint32_t j = 0; j < view.record_count(); ++j) {
      if (keys[j] == Key{kArchiveType, r.name}) archiveIndex = j;
    }
    if (archiveIndex == view.record_count()) return Refuse(Refusal::kLevelIncomplete, KeyText(kArchiveType, r.name));
    Level level;
    const Outcome read = ReadLevel(view, view.RecordAt(archiveIndex), &level);
    if (!read.ok()) return read;
    set->levels.push_back(std::move(level));
  }

  for (const std::unique_ptr<Set>& other : sets_) {
    const std::vector<Key> otherKeys = SortedKeys(*other);
    for (const Key& key : sorted) {
      if (SortedHas(otherKeys, key)) {
        return Refuse(Refusal::kCollides, KeyText(key.first, key.second) + " owner=plugin:" + other->plugin);
      }
    }
  }

  for (std::uint32_t i = 0; i < view.chunk_count(); ++i) {
    const nevr_resource_set::Chunk c = view.ChunkAt(i);
    const std::uint64_t at = fileOffset + c.offset;
    // The game's chunk record holds the file offset as a u32 (docs/engine/asset-system.md, "Manifest").
    if (at + c.compressed > 0xFFFFFFFFULL) {
      return Refuse(Refusal::kFormat, "file_offset_range at index " + std::to_string(i));
    }
    nevr_shadow::Chunk chunk;
    chunk.file_offset = static_cast<std::uint32_t>(at);
    chunk.compressed = c.compressed;
    chunk.decompressed = c.decompressed;
    set->chunks.push_back(chunk);
  }
  for (std::uint32_t i = 0; i < view.record_count(); ++i) {
    const nevr_resource_set::Record r = view.RecordAt(i);
    nevr_shadow::Record record;
    record.type = r.type;
    record.name = r.name;
    record.chunk = r.chunk;
    record.offset_in_chunk = r.offset_in_chunk;
    record.size = r.size;
    record.alignment = r.alignment;
    set->records.push_back(record);
  }
  sets_.push_back(std::move(set));
  return Outcome{};
}

void Registry::Commit(const std::string& plugin) {
  for (const std::unique_ptr<Set>& set : sets_) {
    if (set->plugin == plugin) set->committed = true;
  }
}

std::vector<std::int64_t> Registry::Drop(const std::string& plugin) {
  std::vector<std::int64_t> handles;
  auto keep = sets_.begin();
  for (auto it = sets_.begin(); it != sets_.end(); ++it) {
    if ((*it)->plugin == plugin && !(*it)->committed) {
      handles.push_back((*it)->handle);
    } else {
      if (keep != it) *keep = std::move(*it);
      ++keep;
    }
  }
  sets_.erase(keep, sets_.end());
  return handles;
}

MountReport Registry::Mount(const StockView& stock) {
  MountReport report;
  report.stock_records = stock.records;
  if (mounted_) return report;
  mounted_ = true;

  std::vector<Key> served;  // the keys of the sets mounted so far, sorted
  for (const std::unique_ptr<Set>& owned : sets_) {
    Set& set = *owned;
    if (!set.committed) {
      report.handles_to_close.push_back(set.handle);
      continue;
    }
    Outcome outcome;
    for (const nevr_shadow::Record& r : set.records) {
      if (stock.has(stock.ctx, r.type, r.name)) {
        outcome = Refuse(Refusal::kStockKey, KeyText(r.type, r.name));
        break;
      }
    }
    const std::vector<Key> own = SortedKeys(set);
    auto resolves = [&](const Key& key) {
      return SortedHas(own, key) || SortedHas(served, key) || stock.has(stock.ctx, key.first, key.second);
    };
    for (const Level& level : set.levels) {
      if (!outcome.ok()) break;
      for (std::uint64_t dependency : level.dependencies) {
        if (!resolves(Key{kArchiveType, dependency})) {
          outcome = Refuse(Refusal::kUnresolved, "level " + KeyText(kArchiveType, level.name) + " depends on " +
                                                     KeyText(kArchiveType, dependency));
          break;
        }
      }
      for (const Key& entry : level.entries) {
        if (!outcome.ok()) break;
        if (!resolves(entry)) {
          outcome = Refuse(Refusal::kUnresolved,
                           "level " + KeyText(kArchiveType, level.name) + " lists " + KeyText(entry.first, entry.second));
        }
      }
    }
    if (!outcome.ok()) {
      report.refusals.push_back({set.plugin, set.label, outcome});
      report.handles_to_close.push_back(set.handle);
      ++report.refused;
      continue;
    }
    set.mounted = true;
    mountedOrder_.push_back(&set);
    served.insert(served.end(), own.begin(), own.end());
    std::sort(served.begin(), served.end());
  }

  nevr_shadow::Input input;
  for (std::uint32_t i = 0; i < mountedOrder_.size(); ++i) {
    const Set& set = *mountedOrder_[i];
    const std::uint32_t chunkBase = static_cast<std::uint32_t>(input.chunks.size());
    input.handles.push_back(set.handle);
    for (nevr_shadow::Chunk chunk : set.chunks) {
      chunk.package = i;
      input.chunks.push_back(chunk);
    }
    for (nevr_shadow::Record record : set.records) {
      record.chunk += chunkBase;
      record.user = i;
      input.records.push_back(record);
    }
  }
  if (input.records.empty()) return report;
  report.shadow_status = nevr_shadow::Shadow::Build(input, &shadow_);
  if (report.shadow_status != nevr_shadow::Status::kOk) {
    for (const Set* set : mountedOrder_) {
      report.refusals.push_back({set->plugin, set->label,
                                 Refuse(Refusal::kFormat, std::string("shadow build: ") +
                                                              nevr_shadow::StatusName(report.shadow_status))});
      report.handles_to_close.push_back(set->handle);
      ++report.refused;
    }
    mountedOrder_.clear();
    return report;
  }
  report.sets = mountedOrder_.size();
  report.records = input.records.size();
  for (const Set* set : mountedOrder_) report.levels += set->levels.size();
  report.chunk_base = shadow_->chunk_base();
  return report;
}

std::vector<std::int64_t> Registry::Abandon() {
  std::vector<std::int64_t> handles;
  for (const std::unique_ptr<Set>& set : sets_) handles.push_back(set->handle);
  sets_.clear();
  mountedOrder_.clear();
  shadow_.reset();
  mounted_ = true;
  return handles;
}

std::vector<std::uint64_t> Registry::MountedLevels() const {
  std::vector<std::uint64_t> names;
  for (const Set* set : mountedOrder_) {
    for (const Level& level : set->levels) names.push_back(level.name);
  }
  return names;
}

std::vector<const Set*> Registry::MountedSets() const { return mountedOrder_; }

const Set* Registry::SetForUser(std::uint32_t user) const {
  return user < mountedOrder_.size() ? mountedOrder_[user] : nullptr;
}

}  // namespace nevr_resource_sets
