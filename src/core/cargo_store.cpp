#include "core/cargo_store.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include <nlohmann/json.hpp>

#include "core/cargo_manifest.h"

namespace nevr_cargo {

namespace {

using json = nlohmann::ordered_json;

constexpr std::size_t kReadChunk = 64 * 1024;
constexpr std::size_t kSha256HexLength = 64;
const std::string kBlobSuffix = ".blob";

}  // namespace

std::string FormatHex64(std::uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  std::string out = "0x";
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const unsigned d = static_cast<unsigned>((v >> shift) & 0xf);
    if (d != 0 || started || shift == 0) {
      out.push_back(digits[d]);
      started = true;
    }
  }
  return out;
}

namespace {

bool IsLabel(const std::string& s) noexcept {
  if (s.empty() || s.size() > 48) return false;
  for (const char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

const std::string* StringOf(const json& v) noexcept { return v.get_ptr<const std::string*>(); }
const json::number_unsigned_t* UnsignedOf(const json& v) noexcept { return v.get_ptr<const json::number_unsigned_t*>(); }

// Hashes `size` bytes of an open blob through its handle. False when a read fails or comes up short.
bool HashHandle(Env& env, std::int64_t handle, std::uint64_t size, std::string* hex) {
  std::unique_ptr<Hasher> hasher = env.NewHasher();
  if (!hasher) return false;
  std::string buffer(kReadChunk, '\0');
  std::uint64_t offset = 0;
  while (offset < size) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
    std::size_t got = 0;
    if (!env.ReadBlob(handle, offset, &buffer[0], want, &got) || got != want) return false;
    hasher->Update(buffer.data(), got);
    offset += got;
  }
  *hex = hasher->Hex();
  return true;
}

}  // namespace

std::string BlobPath(const std::string& dir, const std::string& sha256) { return dir + "/" + sha256 + ".blob"; }
std::string IndexPath(const std::string& dir) { return dir + "/" + kIndexFile; }

void Normalize(Index* index) {
  std::sort(index->entries.begin(), index->entries.end(), [](const IndexEntry& a, const IndexEntry& b) {
    return std::tie(a.label, a.type, a.name) < std::tie(b.label, b.type, b.name);
  });
}

const char* IndexStatusName(IndexStatus status) noexcept {
  switch (status) {
    case IndexStatus::kOk: return "ok";
    case IndexStatus::kAbsent: return "absent";
    case IndexStatus::kUnreadable: return "unreadable";
    case IndexStatus::kInvalid: return "invalid";
  }
  return "unknown";
}

IndexStatus ReadIndex(Env& env, const std::string& dir, Index* out) {
  *out = Index();
  std::string text;
  if (!env.ReadSmallFile(IndexPath(dir), &text, kMaxIndexBytes)) return IndexStatus::kAbsent;
  const json doc = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) return IndexStatus::kInvalid;
  const auto version = doc.find("version");
  const json::number_unsigned_t* v = version != doc.end() ? UnsignedOf(*version) : nullptr;
  if (v == nullptr || *v != static_cast<json::number_unsigned_t>(kIndexVersion)) return IndexStatus::kInvalid;
  const auto entries = doc.find("entries");
  if (entries == doc.end() || !entries->is_array()) return IndexStatus::kInvalid;
  Index index;
  std::set<std::tuple<std::string, std::uint64_t, std::uint64_t>> seen;
  for (const json& item : *entries) {
    if (!item.is_object()) return IndexStatus::kInvalid;
    const auto type = item.find("type");
    const auto name = item.find("name");
    const auto sha = item.find("sha256");
    const auto label = item.find("label");
    const auto format = item.find("format");
    const auto bytes = item.find("bytes");
    const auto align = item.find("align");
    if (type == item.end() || name == item.end() || sha == item.end() || label == item.end() || format == item.end() ||
        bytes == item.end() || align == item.end()) {
      return IndexStatus::kInvalid;
    }
    const std::string* typeText = StringOf(*type);
    const std::string* nameText = StringOf(*name);
    const std::string* shaText = StringOf(*sha);
    const std::string* labelText = StringOf(*label);
    const std::string* formatText = StringOf(*format);
    const json::number_unsigned_t* bytesValue = UnsignedOf(*bytes);
    const json::number_unsigned_t* alignValue = UnsignedOf(*align);
    if (typeText == nullptr || nameText == nullptr || shaText == nullptr || labelText == nullptr || formatText == nullptr ||
        bytesValue == nullptr || alignValue == nullptr) {
      return IndexStatus::kInvalid;
    }
    IndexEntry e;
    if (!ParseHex64(*typeText, &e.type) || !ParseHex64(*nameText, &e.name) || !IsSha256Hex(*shaText) || !IsLabel(*labelText) ||
        *formatText != kFormatZstdFrame || *bytesValue == 0 || *bytesValue > kMaxBlobBytes || *alignValue == 0 ||
        *alignValue > kMaxAlignment) {
      return IndexStatus::kInvalid;
    }
    e.sha256 = *shaText;
    e.label = *labelText;
    e.format = *formatText;
    e.bytes = *bytesValue;
    e.align = static_cast<std::uint32_t>(*alignValue);
    if (!seen.insert(std::make_tuple(e.label, e.type, e.name)).second) return IndexStatus::kInvalid;
    index.entries.push_back(std::move(e));
  }
  *out = std::move(index);
  return IndexStatus::kOk;
}

bool WriteIndex(Env& env, const std::string& dir, const Index& index) {
  Index sorted = index;
  Normalize(&sorted);
  json entries = json::array();
  for (const IndexEntry& e : sorted.entries) {
    json item = json::object();
    item["type"] = FormatHex64(e.type);
    item["name"] = FormatHex64(e.name);
    item["sha256"] = e.sha256;
    item["label"] = e.label;
    item["format"] = e.format;
    item["bytes"] = e.bytes;
    item["align"] = e.align;
    entries.push_back(std::move(item));
  }
  json doc = json::object();
  doc["version"] = kIndexVersion;
  doc["entries"] = std::move(entries);
  return env.WriteFileAtomically(IndexPath(dir), doc.dump());
}

std::string HashFile(Env& env, const std::string& path) {
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  if (!env.OpenBlob(path, &handle, &size)) return std::string();
  std::string hex;
  const bool ok = HashHandle(env, handle, size, &hex);
  env.CloseBlob(handle);
  return ok ? hex : std::string();
}

EvictReport Evict(Env& env, const std::string& dir, std::uint64_t cap_bytes, const std::set<std::string>& referenced) {
  EvictReport report;
  std::vector<DirFile> files;
  if (!env.ListFiles(dir, &files)) {
    report.listed = false;
    return report;
  }
  struct Blob {
    std::string sha256;
    std::uint64_t size;
    std::uint64_t modified_ms;
  };
  std::vector<Blob> candidates;  // unreferenced blobs
  for (const DirFile& f : files) {
    const std::size_t suffix = kBlobSuffix.size();
    if (f.name.size() != kSha256HexLength + suffix || f.name.compare(kSha256HexLength, suffix, kBlobSuffix) != 0) continue;
    const std::string sha = f.name.substr(0, kSha256HexLength);
    if (!IsSha256Hex(sha)) continue;
    report.bytes_before += f.size;
    ++report.blobs_before;
    if (referenced.count(sha) == 0) candidates.push_back({sha, f.size, f.modified_ms});
  }
  report.bytes_after = report.bytes_before;
  std::sort(candidates.begin(), candidates.end(), [](const Blob& a, const Blob& b) {
    return std::tie(a.modified_ms, a.sha256) < std::tie(b.modified_ms, b.sha256);
  });
  for (const Blob& b : candidates) {
    if (report.bytes_after <= cap_bytes) break;
    if (env.RemoveFile(BlobPath(dir, b.sha256))) {
      report.bytes_after -= b.size;
      ++report.evicted;
    } else {
      ++report.failed;
    }
  }
  report.over_cap_all_referenced = report.bytes_after > cap_bytes && report.failed == 0;
  return report;
}

Mounted Mount(Env& env, const std::string& dir, const Index& index, const Policy& policy) {
  Mounted result;
  // Group by (label, name): a name's buffers mount together or not at all.
  std::map<std::pair<std::string, std::uint64_t>, std::vector<const IndexEntry*>> groups;
  for (const IndexEntry& e : index.entries) {
    if (StateOf(e.label, policy) == LabelState::kOff) continue;
    groups[std::make_pair(e.label, e.name)].push_back(&e);
  }
  nevr_shadow::Input input;
  std::map<std::string, std::uint32_t> labelIndex;
  for (const auto& group : groups) {
    struct Opened {
      const IndexEntry* entry;
      std::int64_t handle;
      std::uint64_t size;
    };
    std::vector<Opened> opened;
    const char* failure = nullptr;
    for (const IndexEntry* e : group.second) {
      std::int64_t handle = 0;
      std::uint64_t size = 0;
      if (!env.OpenBlob(BlobPath(dir, e->sha256), &handle, &size)) {
        failure = "open_failed";
        break;
      }
      opened.push_back({e, handle, size});
      std::string hex;
      if (!HashHandle(env, handle, size, &hex)) {
        failure = "read_failed";
        break;
      }
      if (hex != e->sha256) {
        failure = "sha256_mismatch";
        break;
      }
      if (size == 0 || size > kMaxBlobBytes) {
        failure = "size";
        break;
      }
    }
    if (failure != nullptr) {
      for (const Opened& o : opened) env.CloseBlob(o.handle);
      ++result.refused;
      env.Event(EventLevel::kWarn, "cargo_mount",
                {{"result", "refused"}, {"label", group.first.first}, {"name", FormatHex64(group.first.second)}, {"reason", failure}});
      continue;
    }
    const auto it = labelIndex.emplace(group.first.first, static_cast<std::uint32_t>(result.labels.size()));
    if (it.second) result.labels.push_back(group.first.first);
    for (const Opened& o : opened) {
      const std::uint32_t package = static_cast<std::uint32_t>(input.handles.size());
      input.handles.push_back(o.handle);
      nevr_shadow::Chunk chunk;
      chunk.package = package;
      chunk.file_offset = 0;
      chunk.compressed = static_cast<std::uint32_t>(o.size);
      chunk.decompressed = static_cast<std::uint32_t>(o.entry->bytes);
      const std::uint32_t chunkIndex = static_cast<std::uint32_t>(input.chunks.size());
      input.chunks.push_back(chunk);
      nevr_shadow::Record rec;
      rec.type = o.entry->type;
      rec.name = o.entry->name;
      rec.chunk = chunkIndex;
      rec.offset_in_chunk = 0;
      rec.size = static_cast<std::uint32_t>(o.entry->bytes);
      rec.alignment = o.entry->align;
      rec.user = it.first->second;
      input.records.push_back(rec);
      ++result.records;
    }
    ++result.names;
  }
  if (input.records.empty()) return result;
  const nevr_shadow::Status status = nevr_shadow::Shadow::Build(input, &result.shadow);
  if (status != nevr_shadow::Status::kOk) {
    for (const std::int64_t h : input.handles) env.CloseBlob(h);
    env.Event(EventLevel::kWarn, "cargo_mount", {{"result", "refused"}, {"reason", nevr_shadow::StatusName(status)}});
    result.refused += result.names;
    result.names = 0;
    result.records = 0;
    result.labels.clear();
  }
  return result;
}

}  // namespace nevr_cargo
