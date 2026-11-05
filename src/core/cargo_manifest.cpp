#include "core/cargo_manifest.h"

#include <set>
#include <tuple>

#include <nlohmann/json.hpp>

namespace nevr_cargo {

namespace {

using json = nlohmann::ordered_json;  // document order matters: the cap is spent in it

// A label is lowercase letters, digits and underscores, 1 to 48 of them: the name is logged, so it is checked before it is used.
bool IsLabelName(const std::string& s) noexcept {
  if (s.empty() || s.size() > 48) return false;
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

// A platform name is lowercase letters, digits, '-' and '_', 1 to 32 of them.
bool IsPlatformName(const std::string& s) noexcept {
  if (s.empty() || s.size() > 32) return false;
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

constexpr const char* kTopKeys[] = {"schema", "schema_version", "labels"};
constexpr const char* kLabelKeys[] = {"entries"};
constexpr const char* kEntryKeys[] = {"platform", "type", "name", "format", "sha256", "size", "bytes", "align", "stock_size"};

ParseResult Fail(ParseStatus status, const std::string& label = std::string(), std::size_t index = 0) {
  ParseResult r;
  r.status = status;
  r.label = label;
  r.index = index;
  return r;
}

// One entry. Fills *e or returns the status.
ParseStatus ReadEntry(const json& v, Entry* e, std::uint32_t* unknown) {
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
      bytes == v.end() || align == v.end() || stock == v.end()) {
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
  const json::number_unsigned_t* stockValue = UnsignedOf(*stock);
  if (platformText == nullptr || typeText == nullptr || nameText == nullptr || formatText == nullptr || shaText == nullptr ||
      sizeValue == nullptr || bytesValue == nullptr || alignValue == nullptr || stockValue == nullptr) {
    return ParseStatus::kBadField;
  }
  if (!IsPlatformName(*platformText) || !ParseHex64(*typeText, &e->type) || !ParseHex64(*nameText, &e->name)) {
    return ParseStatus::kBadField;
  }
  if (!IsSha256Hex(*shaText)) return ParseStatus::kBadHash;
  if (*formatText != kFormatZstdFrame) return ParseStatus::kBadFormat;
  if (*sizeValue == 0 || *sizeValue > kMaxBlobBytes || *bytesValue == 0 || *bytesValue > kMaxBlobBytes) return ParseStatus::kTooBig;
  if (*alignValue == 0 || *alignValue > kMaxAlignment || *stockValue > kMaxBlobBytes) return ParseStatus::kBadField;
  e->platform = *platformText;
  e->sha256 = *shaText;
  e->size = *sizeValue;
  e->bytes = *bytesValue;
  e->align = static_cast<std::uint32_t>(*alignValue);
  e->stock_size = *stockValue;
  return ParseStatus::kOk;
}

}  // namespace

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
    case ParseStatus::kTooBig: return "too_big";
    case ParseStatus::kBadFormat: return "bad_format";
    case ParseStatus::kDuplicate: return "duplicate";
    case ParseStatus::kTooMany: return "too_many";
  }
  return "unknown";
}

bool ParseHex64(std::string_view text, std::uint64_t* out) noexcept {
  if (text.size() < 3 || text.size() > 18 || text[0] != '0' || text[1] != 'x') return false;
  std::uint64_t v = 0;
  for (std::size_t i = 2; i < text.size(); ++i) {
    const char c = text[i];
    unsigned d = 0;
    if (c >= '0' && c <= '9') {
      d = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<unsigned>(c - 'a') + 10;
    } else {
      return false;
    }
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

bool IsSha256Hex(std::string_view text) noexcept {
  if (text.size() != 64) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

ParseResult ParseManifest(std::string_view text, Manifest* out) {
  *out = Manifest();
  if (text.size() > kMaxManifestBytes) return Fail(ParseStatus::kTooLarge);
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
  if (labels->size() > kMaxLabels) return Fail(ParseStatus::kTooMany);
  std::set<std::tuple<std::string, std::uint64_t, std::uint64_t>> seen;
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
      if (m.entries.size() >= kMaxEntries) return Fail(ParseStatus::kTooMany, label, index);
      Entry e;
      e.label = label;
      const ParseStatus status = ReadEntry(item, &e, &m.unknown_fields);
      if (status != ParseStatus::kOk) return Fail(status, label, index);
      if (!seen.insert(std::make_tuple(e.platform, e.type, e.name)).second) return Fail(ParseStatus::kDuplicate, label, index);
      m.entries.push_back(std::move(e));
      ++index;
    }
  }
  *out = std::move(m);
  return ParseResult();
}

}  // namespace nevr_cargo
