#include "core/cargo_manifest.h"

#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <zstd.h>

#include <nlohmann/json.hpp>

#include "core/resource_shadow.h"

namespace nevr_cargo {

namespace {

using json = nlohmann::ordered_json;  // document order matters: refusals are listed in it

// A label is lowercase letters, digits and underscores, at least one: the name is logged, so its characters are checked before it is
// used. No length is fixed by the format (the game names a resource by a 64-bit symbol, a label is ours); the length is bounded by the
// manifest text limit, which comes from the device (cargo_limits.h).
bool IsLabelName(const std::string& s) noexcept {
  if (s.empty()) return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

// A platform name is lowercase letters, digits, '-' and '_', at least one; bounded by the manifest text limit like a label.
bool IsPlatformName(const std::string& s) noexcept {
  if (s.empty()) return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

const std::string* StringOf(const json& v) noexcept { return v.get_ptr<const std::string*>(); }

const json::number_unsigned_t* UnsignedOf(const json& v) noexcept { return v.get_ptr<const json::number_unsigned_t*>(); }

bool IsKnown(const std::string& key, const char* const* known, std::size_t count) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    if (key == known[i]) return true;
  }
  return false;
}

constexpr const char* kTopKeys[] = {"schema", "schema_version", "labels", "empty"};
constexpr const char* kLabelKeys[] = {"entries"};
constexpr const char* kEntryKeys[] = {"platform", "type", "name", "format", "sha256", "size", "bytes", "align", "stock_size"};
constexpr const char* kEmptyKeys[] = {"type", "name", "align"};

ParseResult Fail(ParseStatus status, const std::string& label = std::string(), std::size_t index = 0) {
  ParseResult r;
  r.status = status;
  r.label = label;
  r.index = index;
  return r;
}

// What an entry's JSON says, before the validator has looked at it.
struct Raw {
  Entry entry;
  std::uint64_t align = 0;
};

// One entry. Fills *raw or returns the status. A shape that is not the schema is a status; a value the validator judges is not.
ParseStatus ReadEntry(const json& v, Raw* raw, std::uint32_t* unknown) {
  if (!v.is_object()) return ParseStatus::kBadField;
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (!IsKnown(it.key(), kEntryKeys, sizeof(kEntryKeys) / sizeof(kEntryKeys[0]))) ++*unknown;
  }
  const auto platform = v.find("platform");
  const auto type = v.find("type");
  const auto name = v.find("name");
  const auto format = v.find("format");
  const auto sha = v.find("sha256");
  const auto size = v.find("size");
  const auto bytes = v.find("bytes");
  const auto align = v.find("align");
  const auto stock = v.find("stock_size");
  if (platform == v.end() || type == v.end() || name == v.end() || format == v.end() || sha == v.end() || size == v.end() ||
      bytes == v.end() || align == v.end()) {
    return ParseStatus::kMissingField;
  }
  const std::string* platformText = StringOf(*platform);
  const std::string* typeText = StringOf(*type);
  const std::string* nameText = StringOf(*name);
  const std::string* formatText = StringOf(*format);
  const std::string* shaText = StringOf(*sha);
  const json::number_unsigned_t* sizeValue = UnsignedOf(*size);
  const json::number_unsigned_t* bytesValue = UnsignedOf(*bytes);
  const json::number_unsigned_t* alignValue = UnsignedOf(*align);
  if (platformText == nullptr || typeText == nullptr || nameText == nullptr || formatText == nullptr || shaText == nullptr ||
      sizeValue == nullptr || bytesValue == nullptr || alignValue == nullptr) {
    return ParseStatus::kBadField;
  }
  Entry& e = raw->entry;
  if (stock != v.end()) {
    const json::number_unsigned_t* stockValue = UnsignedOf(*stock);
    if (stockValue == nullptr) return ParseStatus::kBadField;
    e.has_stock_size = true;
    e.stock_size = *stockValue;
  }
  if (!IsPlatformName(*platformText) || !ParseHex64(*typeText, &e.type) || !ParseHex64(*nameText, &e.name)) {
    return ParseStatus::kBadField;
  }
  if (!IsSha256Hex(*shaText)) return ParseStatus::kBadHash;
  if (*formatText != kFormatZstdFrame) return ParseStatus::kBadFormat;
  e.platform = *platformText;
  e.sha256 = *shaText;
  e.size = *sizeValue;
  e.bytes = *bytesValue;
  raw->align = *alignValue;
  return ParseStatus::kOk;
}

// One record of the `empty` list.
ParseStatus ReadEmpty(const json& v, EmptyRecord* e, std::uint64_t* align, std::uint32_t* unknown) {
  if (!v.is_object()) return ParseStatus::kBadField;
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (!IsKnown(it.key(), kEmptyKeys, sizeof(kEmptyKeys) / sizeof(kEmptyKeys[0]))) ++*unknown;
  }
  const auto type = v.find("type");
  const auto name = v.find("name");
  const auto alignIt = v.find("align");
  if (type == v.end() || name == v.end() || alignIt == v.end()) return ParseStatus::kMissingField;
  const std::string* typeText = StringOf(*type);
  const std::string* nameText = StringOf(*name);
  const json::number_unsigned_t* alignValue = UnsignedOf(*alignIt);
  if (typeText == nullptr || nameText == nullptr || alignValue == nullptr) return ParseStatus::kBadField;
  if (!ParseHex64(*typeText, &e->type) || !ParseHex64(*nameText, &e->name)) return ParseStatus::kBadField;
  *align = *alignValue;
  return ParseStatus::kOk;
}

}  // namespace

std::uint64_t ManifestEntryMemoryBytes() noexcept {
  // The entry, the heap its SHA-256 text takes (the hex characters and the terminator: longer than a short string, so out of line), and
  // the shadow's record and chunk for it. The allocator's own overhead is not counted.
  return sizeof(Entry) + (kSha256HexChars + 1) + nevr_shadow::kRecordBytes + nevr_shadow::kChunkBytes;
}

const char* ParseStatusName(ParseStatus status) noexcept {
  switch (status) {
    case ParseStatus::kOk: return "ok";
    case ParseStatus::kTooLarge: return "too_large";
    case ParseStatus::kNotJson: return "not_json";
    case ParseStatus::kSchema: return "schema";
    case ParseStatus::kVersion: return "version";
    case ParseStatus::kMissingField: return "missing_field";
    case ParseStatus::kBadField: return "bad_field";
    case ParseStatus::kBadHash: return "bad_hash";
    case ParseStatus::kBadFormat: return "bad_format";
    case ParseStatus::kTooMany: return "too_many";
    case ParseStatus::kNoLimits: return "no_limits";
    case ParseStatus::kNotZstd: return "not_zstd";
    case ParseStatus::kNoContentSize: return "no_content_size";
    case ParseStatus::kDecompress: return "decompress";
  }
  return "unknown";
}

ParseResult ParseManifest(std::string_view text, const Limits& limits, Manifest* out) {
  *out = Manifest();
  if (!limits.ok) return Fail(ParseStatus::kNoLimits);
  if (text.size() > limits.manifest_text_bytes) return Fail(ParseStatus::kTooLarge);
  const json doc = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) return Fail(ParseStatus::kNotJson);
  const auto schema = doc.find("schema");
  const std::string* schemaText = schema != doc.end() ? StringOf(*schema) : nullptr;
  if (schemaText == nullptr || *schemaText != kSchema) return Fail(ParseStatus::kSchema);
  const auto version = doc.find("schema_version");
  const json::number_unsigned_t* versionValue = version != doc.end() ? UnsignedOf(*version) : nullptr;
  if (versionValue == nullptr || *versionValue != static_cast<json::number_unsigned_t>(kSchemaVersion)) return Fail(ParseStatus::kVersion);
  const auto labels = doc.find("labels");
  if (labels == doc.end()) return Fail(ParseStatus::kMissingField);
  if (!labels->is_object()) return Fail(ParseStatus::kBadField);
  Manifest m;
  for (auto it = doc.begin(); it != doc.end(); ++it) {
    if (!IsKnown(it.key(), kTopKeys, sizeof(kTopKeys) / sizeof(kTopKeys[0]))) ++m.unknown_fields;
  }

  // Pass 1: the shape, and the rules a record carries alone. A record that breaks one is listed and left out.
  struct Candidate {
    Entry entry;
    std::string label;
    std::size_t index;
  };
  std::vector<Candidate> candidates;
  std::uint64_t seen_total = 0;
  KeySet keys;
  std::set<std::tuple<std::string, std::uint64_t, std::uint64_t>> duplicated;
  for (auto lit = labels->begin(); lit != labels->end(); ++lit) {
    const std::string& label = lit.key();
    if (!IsLabelName(label)) return Fail(ParseStatus::kBadField);
    const json& body = lit.value();
    if (!body.is_object()) return Fail(ParseStatus::kBadField, label);
    for (auto kit = body.begin(); kit != body.end(); ++kit) {
      if (!IsKnown(kit.key(), kLabelKeys, sizeof(kLabelKeys) / sizeof(kLabelKeys[0]))) ++m.unknown_fields;
    }
    const auto entries = body.find("entries");
    if (entries == body.end()) return Fail(ParseStatus::kMissingField, label);
    if (!entries->is_array()) return Fail(ParseStatus::kBadField, label);
    m.labels.push_back(label);
    std::size_t index = 0;
    for (const json& item : *entries) {
      if (seen_total >= limits.max_entries) return Fail(ParseStatus::kTooMany, label, index);
      ++seen_total;
      Raw raw;
      raw.entry.label = label;
      const ParseStatus status = ReadEntry(item, &raw, &m.unknown_fields);
      if (status != ParseStatus::kOk) return Fail(status, label, index);
      RecordFields fields;
      fields.size = raw.entry.size;
      fields.bytes = raw.entry.bytes;
      fields.align = raw.align;
      const Rule rule = CheckRecord(fields, limits);
      if (rule != Rule::kNone) {
        m.refused.push_back({label, index, raw.entry.type, raw.entry.name, rule});
      } else {
        raw.entry.align = static_cast<std::uint32_t>(raw.align);
        if (keys.Add(raw.entry.platform, raw.entry.type, raw.entry.name)) {
          duplicated.insert(std::make_tuple(raw.entry.platform, raw.entry.type, raw.entry.name));
        }
        candidates.push_back({std::move(raw.entry), label, index});
      }
      ++index;
    }
  }

  // The records of 0 bytes: the same rules for what they state (alignment), and a key of their own to share with nobody.
  struct EmptyCandidate {
    EmptyRecord record;
    std::size_t index;
  };
  std::vector<EmptyCandidate> empties;
  const auto emptyList = doc.find("empty");
  if (emptyList != doc.end()) {
    if (!emptyList->is_array()) return Fail(ParseStatus::kBadField, "empty");
    std::size_t index = 0;
    KeySet emptyKeys;
    std::set<std::pair<std::uint64_t, std::uint64_t>> emptyDuplicated;
    for (const json& item : *emptyList) {
      if (seen_total >= limits.max_entries) return Fail(ParseStatus::kTooMany, "empty", index);
      ++seen_total;
      EmptyRecord rec;
      std::uint64_t align = 0;
      const ParseStatus status = ReadEmpty(item, &rec, &align, &m.unknown_fields);
      if (status != ParseStatus::kOk) return Fail(status, "empty", index);
      RecordFields fields;
      fields.size = 1;   // a record of 0 bytes has no frame and no stored size: only its alignment is judged here
      fields.bytes = 1;
      fields.align = align;
      const Rule rule = CheckRecord(fields, limits);
      if (rule != Rule::kNone) {
        m.refused.push_back({"empty", index, rec.type, rec.name, rule});
      } else {
        rec.align = static_cast<std::uint32_t>(align);
        if (emptyKeys.Add("", rec.type, rec.name)) emptyDuplicated.insert({rec.type, rec.name});
        empties.push_back({rec, index});
      }
      ++index;
    }
    // Pass 2 for the empty list, against itself and against the entries (a manifest is one platform's set).
    std::set<std::pair<std::uint64_t, std::uint64_t>> entryKeys;
    for (const Candidate& c : candidates) entryKeys.insert({c.entry.type, c.entry.name});
    std::set<std::pair<std::uint64_t, std::uint64_t>> emptyKeySet;
    for (const EmptyCandidate& e : empties) emptyKeySet.insert({e.record.type, e.record.name});
    for (const Candidate& c : candidates) {
      if (emptyKeySet.count({c.entry.type, c.entry.name}) != 0) {
        duplicated.insert(std::make_tuple(c.entry.platform, c.entry.type, c.entry.name));
      }
    }
    for (const EmptyCandidate& e : empties) {
      const bool dup = emptyDuplicated.count({e.record.type, e.record.name}) != 0 || entryKeys.count({e.record.type, e.record.name}) != 0;
      if (dup) {
        m.refused.push_back({"empty", e.index, e.record.type, e.record.name, Rule::kDuplicateKey});
      } else {
        m.empties.push_back(e.record);
      }
    }
  }

  // Pass 2: a key held twice is held by neither: both entries are refused.
  for (Candidate& c : candidates) {
    if (duplicated.count(std::make_tuple(c.entry.platform, c.entry.type, c.entry.name)) != 0) {
      m.refused.push_back({c.label, c.index, c.entry.type, c.entry.name, Rule::kDuplicateKey});
    } else {
      m.entries.push_back(std::move(c.entry));
    }
  }
  *out = std::move(m);
  return ParseResult();
}

std::uint64_t MaxManifestFrameBytes(std::uint64_t declared_bytes) noexcept { return ZSTD_compressBound(static_cast<std::size_t>(declared_bytes)); }

ParseResult ParseManifestFrame(std::string_view frame, const Limits& limits, Manifest* out) {
  *out = Manifest();
  if (!limits.ok) return Fail(ParseStatus::kNoLimits);
  const unsigned char* const bytes = reinterpret_cast<const unsigned char*>(frame.data());
  std::uint64_t declared = 0;
  switch (FrameContentSize(bytes, frame.size() < kMaxFrameHeaderBytes ? frame.size() : kMaxFrameHeaderBytes, &declared)) {
    case Rule::kNone: break;
    case Rule::kContentSizeMissing: return Fail(ParseStatus::kNoContentSize);
    default: return Fail(ParseStatus::kNotZstd);
  }
  // The size the publisher states is held to what this device can give a manifest before a byte is decoded.
  if (declared > limits.manifest_text_bytes) return Fail(ParseStatus::kTooLarge);
  std::string text(static_cast<std::size_t>(declared), '\0');
  const std::size_t got = ZSTD_decompress(&text[0], text.size(), frame.data(), frame.size());
  if (ZSTD_isError(got) != 0 || got != declared) return Fail(ParseStatus::kDecompress);
  return ParseManifest(text, limits, out);
}

}  // namespace nevr_cargo
