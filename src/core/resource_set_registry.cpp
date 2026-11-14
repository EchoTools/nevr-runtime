#include "core/resource_set_registry.h"

#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

#include "core/cargo_format.h"
#include "nevr/resource_set.h"
#include "nevr/symbol_hash.h"
#include "quest/data/sha256.h"

namespace nevr_resource_sets {

static_assert(kWin10Types.archive == EchoVR::CSymbol64Hash("CArchiveResourceWin10"));
static_assert(kWin10Types.level_root == EchoVR::CSymbol64Hash("CGameLevelResourceWin10"));
static_assert(kWin10Types.script == EchoVR::CSymbol64Hash("CScriptResourceWin10"));
static_assert(kWin10Types.texture == EchoVR::CSymbol64Hash("CGTextureResourceWin10"));
static_assert(kWin10Types.texture_gpu == EchoVR::CSymbol64Hash("CGTextureResourceWin10GPU"));
static_assert(kAndroidTypes.archive == EchoVR::CSymbol64Hash("CArchiveResourceAndroid"));
static_assert(kAndroidTypes.level_root == EchoVR::CSymbol64Hash("CGameLevelResourceAndroid"));
static_assert(kAndroidTypes.script == EchoVR::CSymbol64Hash("CScriptResourceAndroid"));
static_assert(kAndroidTypes.texture == EchoVR::CSymbol64Hash("CGTextureResourceAndroid"));
static_assert(kAndroidTypes.texture_gpu == EchoVR::CSymbol64Hash("CGTextureResourceAndroidGPU"));

namespace {

std::atomic<bool (*)(const std::string&, std::uint64_t)> g_scriptWaiver{nullptr};

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

// 1 to `max` bytes of printable ASCII with no double quote or backslash: an owner or a label a log line quotes and the login's JSON
// carries.
bool MapTextOk(const std::string& text, std::size_t max) {
  if (text.empty() || text.size() > max) return false;
  return std::all_of(text.begin(), text.end(), [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= ' ' && u <= '~' && c != '"' && c != '\\';
  });
}

const Archive* FindArchive(const Set& set, std::uint64_t name) {
  const auto it = std::lower_bound(set.archives.begin(), set.archives.end(), name,
                                   [](const Archive& a, std::uint64_t n) { return a.name < n; });
  return it != set.archives.end() && it->name == name ? &*it : nullptr;
}

// One archive record of `view`: decompressed from its chunk and parsed. The parse has already checked that the record
// lies inside its chunk's decompressed bytes and that the frame lies inside the set; an archive record of at most
// kMaxArchiveBytes sits in a chunk of at most kBlockLimit decompressed bytes (Parse), so the buffer is bounded.
Outcome ReadArchive(const nevr_resource_set::View& view, const nevr_resource_set::Record& archive, Archive* out) {
  const nevr_resource_set::Chunk chunk = view.ChunkAt(archive.chunk);
  if (archive.size > kMaxArchiveBytes || chunk.decompressed > nevr_resource_set::kBlockLimit) {
    return Refuse(Refusal::kArchive, KeyText(archive.type, archive.name) + " (an archive of " +
                                         std::to_string(archive.size) + " bytes in a chunk of " +
                                         std::to_string(chunk.decompressed) + "; an archive sits in a chunk of at most " +
                                         std::to_string(nevr_resource_set::kBlockLimit) + ")");
  }
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
    case Refusal::kIndexSpace: return "index_space";
    case Refusal::kFileOffset: return "file_offset_range";
    case Refusal::kSizeLimit: return "size_limit";
    case Refusal::kFrameSize: return "frame_size";
    case Refusal::kHash: return "hash_mismatch";
    case Refusal::kOverride: return "override";
  }
  return "unknown";
}


void SetScriptWaiver(bool (*holds)(const std::string& owner, std::uint64_t name)) { g_scriptWaiver.store(holds); }

std::string KeyText(std::uint64_t type, std::uint64_t name) {
  char text[48];
  std::snprintf(text, sizeof(text), "0x%016llx:0x%016llx", static_cast<unsigned long long>(type),
                static_cast<unsigned long long>(name));
  return text;
}

bool ParseArchive(const unsigned char* data, std::size_t size, Archive* out) {
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
  return AddChecked(plugin, label, bytes, size, handle, fileOffset, false);
}

Outcome Registry::AddMapFile(const std::string& owner, const std::string& label, const void* bytes, std::uint64_t size,
                              std::int64_t handle) {
  if (!IsMapOwner(owner)) return Refuse(Refusal::kFormat, "bad_owner (a map's set is registered for \"map:<id>\")");
  return AddChecked(owner, label, bytes, size, handle, 0, true);
}

Outcome Registry::AddChecked(const std::string& plugin, const std::string& label, const void* bytes, std::uint64_t size,
                             std::int64_t handle, std::uint64_t fileOffset, bool mapFile) {
  if (mounted_) return Refuse(Refusal::kTooLate, "the run's resource sets are already mounted");
  nevr_resource_set::View view;
  const nevr_resource_set::Result parsed = nevr_resource_set::Parse(bytes, size, &view);
  if (!parsed.ok()) {
    return Refuse(Refusal::kFormat, std::string(nevr_resource_set::ProblemName(parsed.problem)) + " at index " +
                                        std::to_string(parsed.index));
  }
  if (mapFile && view.override_count() != 0) {
    const nevr_resource_set::Override o = view.OverrideAt(0);
    return Refuse(Refusal::kOverride, KeyText(o.type, o.name) + " (" + std::to_string(view.override_count()) +
                                          " override records; a map's set only adds resources)");
  }

  // Each chunk: at most kMaxResourceBytes decompressed, and its zstd frame header states exactly that many bytes (the
  // header only; no frame is decompressed here). The game's reader allocates the declared size and decompresses into
  // it, so a frame that holds another size would leave bytes of a resource unwritten or fail the read.
  for (std::uint32_t i = 0; i < view.chunk_count(); ++i) {
    const nevr_resource_set::Chunk c = view.ChunkAt(i);
    if (c.decompressed > kMaxResourceBytes) {
      return Refuse(Refusal::kSizeLimit, "chunk " + std::to_string(i) + " decompresses to " +
                                             std::to_string(c.decompressed) + " bytes, over " +
                                             std::to_string(kMaxResourceBytes));
    }
    const unsigned long long stated = ZSTD_getFrameContentSize(view.data() + c.offset, c.compressed);
    if (stated != c.decompressed) {
      const bool none = stated == ZSTD_CONTENTSIZE_UNKNOWN || stated == ZSTD_CONTENTSIZE_ERROR;
      return Refuse(Refusal::kFrameSize, "chunk " + std::to_string(i) + " declares " +
                                             std::to_string(c.decompressed) + " bytes and its frame header " +
                                             (none ? std::string("states none") : "states " + std::to_string(stated)));
    }
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
  set->committed = mapFile;  // a map's set has no init to wait for; a plugin's Commit and Drop never reach it
  for (std::uint32_t i = 0; i < view.record_count(); ++i) {
    const nevr_resource_set::Record r = view.RecordAt(i);
    if (r.type == types_.script) {
      const auto holds = mapFile ? g_scriptWaiver.load() : nullptr;
      if (holds == nullptr || !holds(plugin, r.name)) return Refuse(Refusal::kScript, KeyText(r.type, r.name));
    }
    if (r.type == types_.texture && !SortedHas(sorted, Key{types_.texture_gpu, r.name})) {
      return Refuse(Refusal::kTexturePayload, KeyText(r.type, r.name));
    }
    if (r.type == types_.archive) {
      Archive archive;
      const Outcome read = ReadArchive(view, r, &archive);
      if (!read.ok()) return read;
      set->archives.push_back(std::move(archive));
    }
    if (r.type == types_.level_root) {
      if (!SortedHas(sorted, Key{types_.archive, r.name})) {
        return Refuse(Refusal::kLevelIncomplete, KeyText(types_.archive, r.name));
      }
      set->levels.push_back(r.name);
    }
  }
  std::sort(set->archives.begin(), set->archives.end(),
            [](const Archive& a, const Archive& b) { return a.name < b.name; });

  for (const std::unique_ptr<Set>& other : sets_) {
    const std::vector<Key> otherKeys = SortedKeys(*other);
    for (const Key& key : sorted) {
      if (SortedHas(otherKeys, key)) {
        return Refuse(Refusal::kCollides, KeyText(key.first, key.second) + " owner=plugin:" + other->plugin);
      }
    }
    // Two sets' overrides of one key: the override registry keeps the first, so the second would be dropped silently.
    for (std::uint32_t i = 0; i < view.override_count(); ++i) {
      const nevr_resource_set::Override o = view.OverrideAt(i);
      for (const OverrideRef& theirs : other->overrides) {
        if (theirs.type == o.type && theirs.name == o.name) {
          return Refuse(Refusal::kCollides,
                        KeyText(o.type, o.name) + " (an override) owner=plugin:" + other->plugin);
        }
      }
    }
  }

  // The shadow gives every mounted chunk an index from kFirstChunkIndex to kChunkIndexLimit; a set whose chunks do not
  // fit beside the ones already held is refused here, alone, rather than every set at the mount.
  if (heldChunks_ + view.chunk_count() > indexSpace_) {
    return Refuse(Refusal::kIndexSpace, std::to_string(view.chunk_count()) + " chunks, " +
                                            std::to_string(indexSpace_ - std::min(heldChunks_, indexSpace_)) + " left");
  }

  for (std::uint32_t i = 0; i < view.chunk_count(); ++i) {
    const nevr_resource_set::Chunk c = view.ChunkAt(i);
    const std::uint64_t at = fileOffset + c.offset;
    // The game's chunk record holds the file offset as a u32 (docs/engine/asset-system.md, "Manifest").
    if (at + c.compressed > 0xFFFFFFFFULL) {
      return Refuse(Refusal::kFileOffset, "chunk " + std::to_string(i) + " ends at byte " +
                                              std::to_string(at + c.compressed) + " of the plugin's file");
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
  for (std::uint32_t i = 0; i < view.override_count(); ++i) {
    const nevr_resource_set::Override o = view.OverrideAt(i);
    set->overrides.push_back(OverrideRef{o.type, o.name, view.data() + o.offset, o.size});
  }
  sets_.push_back(std::move(set));
  heldChunks_ += view.chunk_count();  // after the push, which can throw: a set not held takes no indices
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
      heldChunks_ -= (*it)->chunks.size();
    } else {
      if (keep != it) *keep = std::move(*it);
      ++keep;
    }
  }
  sets_.erase(keep, sets_.end());
  return handles;
}

std::vector<std::int64_t> Registry::DropMap(const std::string& owner) {
  std::vector<std::int64_t> handles;
  if (mounted_ || !IsMapOwner(owner)) return handles;
  auto keep = sets_.begin();
  for (auto it = sets_.begin(); it != sets_.end(); ++it) {
    if ((*it)->plugin == owner) {
      handles.push_back((*it)->handle);
      heldChunks_ -= (*it)->chunks.size();
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
  std::vector<const Set*> servedSets;
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
    // An archive a set (this one or one mounted before it) carries, or null: the game's archives are the game's.
    auto archiveOf = [&](std::uint64_t name) -> const Archive* {
      for (const Set* holder : servedSets) {
        if (const Archive* a = FindArchive(*holder, name)) return a;
      }
      return FindArchive(set, name);
    };
    // Each level: its archive and every archive it depends on, through the sets, must list only what resolves. An
    // archive the game has ends the walk there: the game's own archives resolve in the game's packages.
    for (const std::uint64_t level : set.levels) {
      if (!outcome.ok()) break;
      std::vector<std::uint64_t> pending{level};
      std::vector<std::uint64_t> seen;
      while (!pending.empty() && outcome.ok()) {
        const std::uint64_t name = pending.back();
        pending.pop_back();
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
        seen.push_back(name);
        const Archive* archive = archiveOf(name);
        if (archive == nullptr) {
          if (!stock.has(stock.ctx, types_.archive, name)) {
            outcome = Refuse(Refusal::kUnresolved, "level " + KeyText(types_.archive, level) + " depends on " +
                                                       KeyText(types_.archive, name));
          }
          continue;
        }
        for (const Key& entry : archive->entries) {
          if (!resolves(entry)) {
            outcome = Refuse(Refusal::kUnresolved, "level " + KeyText(types_.archive, level) + ": archive " +
                                                       KeyText(types_.archive, name) + " lists " +
                                                       KeyText(entry.first, entry.second));
            break;
          }
        }
        for (std::uint64_t dependency : archive->dependencies) pending.push_back(dependency);
      }
    }
    if (!outcome.ok()) {
      report.refusals.push_back({set.plugin, set.label, outcome});
      report.handles_to_close.push_back(set.handle);
      ++report.refused;
      continue;
    }
    mountedOrder_.push_back(&set);
    servedSets.push_back(&set);
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
  if (input.records.empty()) {
    KeepOnlyMounted();
    return report;
  }
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
    KeepOnlyMounted();
    return report;
  }
  KeepOnlyMounted();
  report.sets = mountedOrder_.size();
  report.records = input.records.size();
  for (const Set* set : mountedOrder_) report.levels += set->levels.size();
  report.chunk_base = shadow_->chunk_base();
  return report;
}

void Registry::KeepOnlyMounted() {
  std::vector<std::unique_ptr<Set>> kept;
  for (std::unique_ptr<Set>& set : sets_) {
    if (std::find(mountedOrder_.begin(), mountedOrder_.end(), set.get()) == mountedOrder_.end()) continue;
    kept.push_back(std::move(set));  // the Set itself does not move: mountedOrder_'s pointers stay good
  }
  sets_ = std::move(kept);
}

std::vector<std::int64_t> Registry::Abandon() {
  std::vector<std::int64_t> handles;
  for (const std::unique_ptr<Set>& set : sets_) handles.push_back(set->handle);
  sets_.clear();
  mountedOrder_.clear();
  shadow_.reset();
  heldChunks_ = 0;
  mounted_ = true;
  return handles;
}

std::vector<std::uint64_t> Registry::MountedLevels() const {
  std::vector<std::uint64_t> names;
  for (const Set* set : mountedOrder_) names.insert(names.end(), set->levels.begin(), set->levels.end());
  return names;
}

bool Registry::Holds(std::uint64_t type, std::uint64_t name) const {
  for (const std::unique_ptr<Set>& set : sets_) {
    for (const nevr_shadow::Record& r : set->records) {
      if (r.type == type && r.name == name) return true;
    }
  }
  return false;
}

std::vector<const Set*> Registry::MountedSets() const { return mountedOrder_; }

std::vector<const Set*> Registry::HeldSets() const {
  std::vector<const Set*> held;
  for (const std::unique_ptr<Set>& set : sets_) held.push_back(set.get());
  return held;
}

const Set* Registry::SetForUser(std::uint32_t user) const {
  return user < mountedOrder_.size() ? mountedOrder_[user] : nullptr;
}

Outcome CheckMapFileRequest(const MapFileRequest& request) {
  if (!IsMapOwner(request.owner) || !MapTextOk(request.owner, kMaxMapOwnerBytes)) return Refuse(Refusal::kFormat, "bad_owner");
  if (!MapTextOk(request.label, kMaxMapLabelBytes)) return Refuse(Refusal::kFormat, "bad_label");
  if (request.rehash && !nevr_cargo::IsSha256Hex(request.sha256)) return Refuse(Refusal::kFormat, "bad_pin");
  return Outcome{};
}

Outcome RegisterMapFile(const MapFileRequest& request, HeldMapFile& file, const std::function<bool()>& mounted,
                        const std::function<Outcome(const void* view, std::uint64_t size)>& add) {
  const Outcome checked = CheckMapFileRequest(request);
  if (!checked.ok()) return checked;
  // Before the size, the hash and the view, which are wasted once the sets are mounted; `add` checks again under its lock.
  if (mounted()) return Refuse(Refusal::kTooLate, "the run's resource sets are already mounted");
  std::uint64_t size = 0;
  std::string problem;
  if (!file.Size(&size, &problem)) return Refuse(Refusal::kFormat, problem);
  if (size == 0) return Refuse(Refusal::kFormat, "empty_file");
  if (request.rehash) {
    std::vector<unsigned char> step(static_cast<std::size_t>(std::min<std::uint64_t>(size, kRehashStepBytes)));
    nevr_quest::data::Sha256 sha;
    for (std::uint64_t at = 0; at < size;) {
      std::size_t got = 0;
      const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(step.size(), size - at));
      if (!file.ReadAt(at, step.data(), want, &got, &problem)) return Refuse(Refusal::kFormat, "hash_read: " + problem);
      if (got == 0) return Refuse(Refusal::kFormat, "hash_read: the file ended at byte " + std::to_string(at));
      sha.Update(step.data(), got);
      at += got;
    }
    const std::string actual = sha.Hex();
    if (actual != request.sha256) return Refuse(Refusal::kHash, "sha256 " + actual + ", pinned " + request.sha256);
  }
  const void* view = file.Map(size, &problem);
  if (view == nullptr) return Refuse(Refusal::kFormat, problem);
  struct Release {
    HeldMapFile& file;
    const void* view;
    std::uint64_t size;
    ~Release() { file.Unmap(view, size); }
  } release{file, view, size};
  return add(view, size);
}

bool MountObserver::Notify(const MountReport& report) {
  const Fn fn = fn_.load();
  if (fn == nullptr || notified_.exchange(true)) return false;
  fn(report);
  return true;
}

void MountObserver::Reset() {
  fn_.store(nullptr);
  notified_.store(false);
}

}  // namespace nevr_resource_sets
