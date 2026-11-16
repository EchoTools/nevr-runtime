#include "cdn/cdn_manifest.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <zstd.h>

#include <nlohmann/json.hpp>

#include "core/resource_shadow.h"

namespace nevr_cdn {

namespace {

using json = nlohmann::ordered_json;  // document order matters: refusals are listed in it

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
constexpr const char* kLabelKeys[] = {"entries", "empty", "scripts"};
constexpr const char* kEntryKeys[] = {"platform", "type", "name", "format", "sha256", "size", "bytes", "align", "stock_size"};
constexpr const char* kEmptyKeys[] = {"type", "name", "align"};
constexpr const char* kScriptKeys[] = {"name", "platform", "stock_source", "stock_sha256"};
// The fields of a frame: a `scripts` entry that carries one is shipping a script image (Rule::kImageEntry).
constexpr const char* kImageKeys[] = {"type", "format", "sha256", "size", "bytes", "align"};
constexpr const char* kLabelEmptyKeys[] = {"platform", "type", "name", "align"};
// Two holders of one (type, name) on one platform are a duplicate: a key held twice is held by neither.
constexpr std::size_t kDuplicateHolders = 2;

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

// One record of an `empty` list. In a label's own list (`inLabel`) the record names its platform, which it must; a top-level record
// names none.
ParseStatus ReadEmpty(const json& v, bool inLabel, EmptyRecord* e, std::uint64_t* align, std::uint32_t* unknown) {
  if (!v.is_object()) return ParseStatus::kBadField;
  const char* const* known = inLabel ? kLabelEmptyKeys : kEmptyKeys;
  const std::size_t knownCount = inLabel ? sizeof(kLabelEmptyKeys) / sizeof(kLabelEmptyKeys[0]) : sizeof(kEmptyKeys) / sizeof(kEmptyKeys[0]);
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (!IsKnown(it.key(), known, knownCount)) ++*unknown;
  }
  const auto platform = v.find("platform");
  const auto type = v.find("type");
  const auto name = v.find("name");
  const auto alignIt = v.find("align");
  if ((inLabel && platform == v.end()) || type == v.end() || name == v.end() || alignIt == v.end()) return ParseStatus::kMissingField;
  if (inLabel) {
    const std::string* platformText = StringOf(*platform);
    if (platformText == nullptr || !IsPlatformName(*platformText)) return ParseStatus::kBadField;
    e->platform = *platformText;
  }
  const std::string* typeText = StringOf(*type);
  const std::string* nameText = StringOf(*name);
  const json::number_unsigned_t* alignValue = UnsignedOf(*alignIt);
  if (typeText == nullptr || nameText == nullptr || alignValue == nullptr) return ParseStatus::kBadField;
  if (!ParseHex64(*typeText, &e->type) || !ParseHex64(*nameText, &e->name)) return ParseStatus::kBadField;
  *align = *alignValue;
  return ParseStatus::kOk;
}

// One entry of a label's `scripts` list. kNone with *script filled, or the rule it breaks (cdn_validate.h): the first of rekey_edits,
// image_entry, bad_script, stock_hash, in that order. s->name is set as soon as it parses, so a refusal can name it.
Rule ReadScript(const json& v, ScriptEntry* s, std::uint32_t* unknown) {
  if (!v.is_object()) return Rule::kBadScript;
  if (v.find("edits") != v.end()) return Rule::kRekeyEdits;
  for (const char* key : kImageKeys) {
    if (v.find(key) != v.end()) return Rule::kImageEntry;
  }
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (!IsKnown(it.key(), kScriptKeys, sizeof(kScriptKeys) / sizeof(kScriptKeys[0]))) ++*unknown;
  }
  const auto name = v.find("name");
  const auto platform = v.find("platform");
  const auto source = v.find("stock_source");
  const auto sha = v.find("stock_sha256");
  if (name != v.end() && StringOf(*name) != nullptr) ParseHex64(*StringOf(*name), &s->name);  // so a refusal can name it
  if (name == v.end() || platform == v.end() || source == v.end() || sha == v.end()) return Rule::kBadScript;
  const std::string* nameText = StringOf(*name);
  const std::string* platformText = StringOf(*platform);
  const std::string* sourceText = StringOf(*source);
  const std::string* shaText = StringOf(*sha);
  if (nameText == nullptr || platformText == nullptr || sourceText == nullptr || shaText == nullptr) return Rule::kBadScript;
  std::uint64_t parsedName = 0;
  if (!ParseHex64(*nameText, &parsedName)) return Rule::kBadScript;
  if (!IsPlatformName(*platformText) || !ParseHex64(*sourceText, &s->stock_source) || !IsSha256Hex(*shaText)) return Rule::kBadScript;
  if (s->name == s->stock_source) return Rule::kStockHash;
  s->platform = *platformText;
  s->stock_sha256 = *shaText;
  return Rule::kNone;
}

// The rule a record of 0 bytes breaks by what it states: only its alignment (it has no frame and no stored size).
Rule CheckEmpty(std::uint64_t align, const Limits& limits) {
  RecordFields fields;
  fields.size = 1;
  fields.bytes = 1;
  fields.align = align;
  return CheckRecord(fields, limits);
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
  struct EmptyCandidate {
    EmptyRecord record;
    std::string label;  // the refusal's label: the label whose list holds the record, or "empty" for a top-level one
    std::size_t index = 0;
  };
  std::vector<Candidate> candidates;
  std::vector<EmptyCandidate> empties;
  std::set<std::string> refusedLabels;  // a label whose `scripts` list broke a rule: left out whole
  std::uint64_t seen_total = 0;
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
        candidates.push_back({std::move(raw.entry), label, index});
      }
      ++index;
    }
    // The label's `scripts` list: a rule broken by any entry refuses the label whole, and every entry is still judged and listed.
    const auto scriptList = body.find("scripts");
    if (scriptList != body.end()) {
      if (!scriptList->is_array()) return Fail(ParseStatus::kBadField, label);
      std::vector<ScriptEntry> labelScripts;
      std::set<std::pair<std::string, std::uint64_t>> seenScripts;
      std::size_t scriptIndex = 0;
      for (const json& item : *scriptList) {
        if (seen_total >= limits.max_entries) return Fail(ParseStatus::kTooMany, label, scriptIndex);
        ++seen_total;
        ScriptEntry script;
        script.label = label;
        Rule rule = ReadScript(item, &script, &m.unknown_fields);
        if (rule == Rule::kNone && !seenScripts.insert(std::make_pair(script.platform, script.name)).second) rule = Rule::kScriptDuplicate;
        if (rule != Rule::kNone) {
          m.refused.push_back({label, scriptIndex, 0, script.name, rule});
          refusedLabels.insert(label);
        } else {
          labelScripts.push_back(std::move(script));
        }
        ++scriptIndex;
      }
      if (refusedLabels.count(label) == 0) {
        for (ScriptEntry& script : labelScripts) m.scripts.push_back(std::move(script));
      }
    }
    // The label's own list of records of 0 bytes: each names its platform, and belongs to this label.
    const auto labelEmpty = body.find("empty");
    if (labelEmpty == body.end()) continue;
    if (!labelEmpty->is_array()) return Fail(ParseStatus::kBadField, label);
    std::size_t emptyIndex = 0;
    for (const json& item : *labelEmpty) {
      if (seen_total >= limits.max_entries) return Fail(ParseStatus::kTooMany, label, emptyIndex);
      ++seen_total;
      EmptyRecord rec;
      rec.label = label;
      std::uint64_t align = 0;
      const ParseStatus status = ReadEmpty(item, true, &rec, &align, &m.unknown_fields);
      if (status != ParseStatus::kOk) return Fail(status, label, emptyIndex);
      const Rule rule = CheckEmpty(align, limits);
      if (rule != Rule::kNone) {
        m.refused.push_back({label, emptyIndex, rec.type, rec.name, rule});
      } else {
        rec.align = static_cast<std::uint32_t>(align);
        empties.push_back({rec, label, emptyIndex});
      }
      ++emptyIndex;
    }
  }

  // The top-level list, the older shape. It belongs to the one label of a manifest that has exactly one; with two or more labels (or
  // none) no label is named, so each record is refused.
  const auto emptyList = doc.find("empty");
  if (emptyList != doc.end()) {
    if (!emptyList->is_array()) return Fail(ParseStatus::kBadField, "empty");
    std::size_t index = 0;
    for (const json& item : *emptyList) {
      if (seen_total >= limits.max_entries) return Fail(ParseStatus::kTooMany, "empty", index);
      ++seen_total;
      EmptyRecord rec;
      std::uint64_t align = 0;
      const ParseStatus status = ReadEmpty(item, false, &rec, &align, &m.unknown_fields);
      if (status != ParseStatus::kOk) return Fail(status, "empty", index);
      const Rule rule = CheckEmpty(align, limits);
      if (rule != Rule::kNone) {
        m.refused.push_back({"empty", index, rec.type, rec.name, rule});
      } else if (m.labels.size() != 1) {
        m.refused.push_back({"empty", index, rec.type, rec.name, Rule::kUnscopedEmpty});
      } else {
        rec.label = m.labels[0];
        rec.align = static_cast<std::uint32_t>(align);
        empties.push_back({rec, "empty", index});
      }
      ++index;
    }
  }

  // A refused label is left out whole: its entries and records of 0 bytes are neither judged for duplicates nor listed again.
  if (!refusedLabels.empty()) {
    const auto refused = [&refusedLabels](const std::string& label) { return refusedLabels.count(label) != 0; };
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&refused](const Candidate& c) { return refused(c.entry.label); }),
                     candidates.end());
    empties.erase(std::remove_if(empties.begin(), empties.end(), [&refused](const EmptyCandidate& e) { return refused(e.record.label); }),
                  empties.end());
    m.labels.erase(std::remove_if(m.labels.begin(), m.labels.end(), refused), m.labels.end());
  }

  // Pass 2: a key held twice is held by neither. A holder is an entry or a record of 0 bytes, under its (type, name). Two holders of
  // one key conflict when they name the same platform, or when either names none: a top-level record belongs to every platform of
  // its label, so it conflicts with every holder of its key.
  using Key = std::pair<std::uint64_t, std::uint64_t>;
  std::map<Key, std::map<std::string, std::size_t>> holders;
  for (const Candidate& c : candidates) ++holders[Key(c.entry.type, c.entry.name)][c.entry.platform];
  for (const EmptyCandidate& e : empties) ++holders[Key(e.record.type, e.record.name)][e.record.platform];
  const auto conflicts = [&holders](const std::string& platform, const Key& key) {
    const std::map<std::string, std::size_t>& byPlatform = holders.find(key)->second;
    if (platform.empty()) {
      std::size_t total = 0;
      for (const auto& kv : byPlatform) total += kv.second;
      return total >= kDuplicateHolders;
    }
    const auto same = byPlatform.find(platform);
    const auto any = byPlatform.find(std::string());
    const std::size_t count = (same != byPlatform.end() ? same->second : 0) + (any != byPlatform.end() ? any->second : 0);
    return count >= kDuplicateHolders;
  };
  for (EmptyCandidate& e : empties) {
    if (conflicts(e.record.platform, Key(e.record.type, e.record.name))) {
      m.refused.push_back({e.label, e.index, e.record.type, e.record.name, Rule::kDuplicateKey});
    } else {
      m.empties.push_back(e.record);
    }
  }
  for (Candidate& c : candidates) {
    if (conflicts(c.entry.platform, Key(c.entry.type, c.entry.name))) {
      m.refused.push_back({c.label, c.index, c.entry.type, c.entry.name, Rule::kDuplicateKey});
    } else {
      m.entries.push_back(std::move(c.entry));
    }
  }
  *out = std::move(m);
  return ParseResult();
}

std::uint64_t MaxManifestFrameBytes(std::uint64_t declared_bytes) noexcept { return ZSTD_compressBound(static_cast<std::size_t>(declared_bytes)); }

bool DecodeFrame(std::string_view frame, std::string* text) {
  const unsigned char* const bytes = reinterpret_cast<const unsigned char*>(frame.data());
  std::uint64_t declared = 0;
  if (FrameContentSize(bytes, frame.size() < kMaxFrameHeaderBytes ? frame.size() : kMaxFrameHeaderBytes, &declared) != Rule::kNone) {
    return false;
  }
  text->assign(static_cast<std::size_t>(declared), '\0');
  const std::size_t got = ZSTD_decompress(&(*text)[0], text->size(), frame.data(), frame.size());
  return ZSTD_isError(got) == 0 && got == declared;
}

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
  std::string text;
  if (!DecodeFrame(frame, &text)) return Fail(ParseStatus::kDecompress);
  return ParseManifest(text, limits, out);
}

bool StartsWithZstdMagic(std::string_view body) noexcept {
  if (body.size() < kZstdMagicBytes) return false;
  for (std::size_t i = 0; i < kZstdMagicBytes; ++i) {
    if (static_cast<unsigned char>(body[i]) != kZstdMagic[i]) return false;
  }
  return true;
}

ParseResult ParseManifestBody(std::string_view body, const Limits& limits, Manifest* out) {
  return StartsWithZstdMagic(body) ? ParseManifestFrame(body, limits, out) : ParseManifest(body, limits, out);
}

}  // namespace nevr_cdn
