#include "core/map_manifest.h"

#include <nevr/symbol_hash.h>

#include <algorithm>
#include <iterator>
#include <set>

#include <nlohmann/json.hpp>

#include "core/cargo_format.h"
#include "core/cargo_limits.h"
#include "core/device_memory.h"
#include "quest/data/sha256.h"

namespace nevr_maps {

namespace {

using json = nlohmann::json;
using nevr_cargo::IsSha256Hex;
using nevr_cargo::ParseHex64;

// Non-throwing parse that also refuses duplicate keys and deep nesting: two readers must never disagree on a signed body.
bool ParseStrict(std::string_view text, json* out) {
  bool bad = false;
  std::vector<std::set<std::string>> open_objects;
  const json::parser_callback_t check = [&](int depth, json::parse_event_t event, json& parsed) {
    switch (event) {
      case json::parse_event_t::object_start:
        if (depth > kMaxJsonDepth) bad = true;
        open_objects.emplace_back();
        break;
      case json::parse_event_t::array_start:
        if (depth > kMaxJsonDepth) bad = true;
        break;
      case json::parse_event_t::object_end:
        if (!open_objects.empty()) open_objects.pop_back();
        break;
      case json::parse_event_t::key: {
        const std::string* key = parsed.get_ptr<const std::string*>();
        if (key == nullptr || open_objects.empty() || !open_objects.back().insert(*key).second) bad = true;
        break;
      }
      default:
        break;
    }
    return true;
  };
  *out = json::parse(text.begin(), text.end(), check, /*allow_exceptions=*/false);
  return !bad && !out->is_discarded() && out->is_object();
}

const json* Find(const json& object, const char* key) {
  const auto it = object.find(key);
  return it == object.end() ? nullptr : &*it;
}

const std::string* StringAt(const json& object, const char* key) {
  const json* v = Find(object, key);
  return v == nullptr ? nullptr : v->get_ptr<const std::string*>();
}

bool UnsignedAt(const json& object, const char* key, std::uint64_t* out) {
  const json* v = Find(object, key);
  const json::number_unsigned_t* n = v == nullptr ? nullptr : v->get_ptr<const json::number_unsigned_t*>();
  if (n == nullptr) return false;
  *out = *n;
  return true;
}

bool Hex64At(const json& object, const char* key, std::uint64_t* out) {
  const std::string* s = StringAt(object, key);
  return s != nullptr && ParseHex64(*s, out);
}

// A script's symbol as its member path writes it: 16 lowercase hex digits, zero padded.
std::string Hash16(std::uint64_t value) {
  std::string out(nevr_cargo::kSymbolHexDigits, '0');
  for (std::size_t i = nevr_cargo::kSymbolHexDigits; i-- > 0; value >>= nevr_cargo::kHexDigitBits) {
    out[i] = nevr_cargo::kHexDigits[value & nevr_cargo::kHexDigitMask];
  }
  return out;
}

// "" when *pin was read; else the reason.
std::string ReadPin(const json* v, std::uint64_t max_size, Pin* pin) {
  if (v == nullptr || !v->is_object()) return "malformed";
  const std::string* sha = StringAt(*v, "sha256");
  if (sha == nullptr || !UnsignedAt(*v, "size", &pin->size)) return "malformed";
  if (!IsSha256Hex(*sha)) return "bad_hash";
  if (pin->size == 0 || pin->size > max_size) return "malformed";
  pin->sha256 = *sha;
  return "";
}

bool IsSymbolName(std::string_view name) {
  if (name.empty() || name.size() > kMaxSymbolNameChars) return false;
  return std::all_of(name.begin(), name.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::uint64_t SymbolOf(const std::string& name) { return EchoVR::CSymbol64Hash(name.c_str()); }

std::string ExpectedLevelName(const std::string& id, const std::string& platform) {
  return platform == kPlatformQuest ? id + kQuestLevelSuffix : id;
}

// Splits a valid version into its parts; false for an invalid one.
bool VersionParts(std::string_view text, std::vector<std::uint32_t>* parts) {
  parts->clear();
  std::size_t at = 0;
  while (true) {
    const std::size_t dot = text.find('.', at);
    const std::string_view part = text.substr(at, dot == std::string_view::npos ? std::string_view::npos : dot - at);
    if (part.empty() || part.size() > kMaxVersionDigits || (part.size() > 1 && part[0] == '0')) return false;
    std::uint32_t value = 0;
    for (const char c : part) {
      if (c < '0' || c > '9') return false;
      value = value * kDecimalBase + static_cast<std::uint32_t>(c - '0');
    }
    parts->push_back(value);
    if (parts->size() > kMaxVersionParts) return false;
    if (dot == std::string_view::npos) return true;
    at = dot + 1;
  }
}

const char* SignatureReason(nevr_minisign::Verdict verdict) {
  switch (verdict) {
    case nevr_minisign::Verdict::kOk: return "";
    case nevr_minisign::Verdict::kMalformed: return "malformed";
    case nevr_minisign::Verdict::kUnsupported: return "unsupported_signature";
    case nevr_minisign::Verdict::kUnknownKey: return "unknown_key";
    case nevr_minisign::Verdict::kBadSignature: return "bad_signature";
    case nevr_minisign::Verdict::kBadGlobalSignature: return "bad_global_signature";
  }
  return "malformed";
}

std::string ReadPlatformEntry(const json& v, const std::string& id, const std::string& platform, PlatformEntry* e) {
  if (!v.is_object()) return "malformed";
  const std::string* version = StringAt(v, "version");
  const std::string* level_name = StringAt(v, "level_name");
  const std::string* game_build = StringAt(v, "game_build");
  std::uint64_t scripts = 0;
  if (version == nullptr || level_name == nullptr || game_build == nullptr || !Hex64At(v, "level", &e->level) ||
      !UnsignedAt(v, "bytes", &e->bytes) || !UnsignedAt(v, "scripts", &scripts) || scripts > UINT32_MAX) {
    return "malformed";
  }
  if (!IsValidVersion(*version)) return "bad_version";
  // The v1 limit, not this device's share: every device accepts the same manifest, and an index too large to read here is
  // refused when it is read (ParsePackageIndex).
  const std::string reason = ReadPin(Find(v, "index"), kMaxIndexBytes, &e->index);
  if (!reason.empty()) return reason;
  if (*level_name != ExpectedLevelName(id, platform) || e->level != SymbolOf(*level_name)) return "bad_level";
  e->version = *version;
  e->level_name = *level_name;
  e->game_build = *game_build;
  e->scripts = static_cast<std::uint32_t>(scripts);
  return "";
}

std::string ReadMapEntry(const json& v, const std::string& id, MapEntry* m) {
  if (!v.is_object()) return "malformed";
  for (const char* key : {"title", "author"}) {
    const json* text = Find(v, key);
    if (text != nullptr && !text->is_string()) return "malformed";
  }
  if (const std::string* title = StringAt(v, "title")) m->title = *title;
  if (const std::string* author = StringAt(v, "author")) m->author = *author;
  if (const json* thumbnail = Find(v, "thumbnail")) {
    const std::string reason = ReadPin(thumbnail, kMaxPieceBytes, &m->thumbnail);
    if (!reason.empty()) return reason;
  }
  const json* platforms = Find(v, "platforms");
  if (platforms == nullptr || !platforms->is_object()) return "malformed";
  for (auto it = platforms->begin(); it != platforms->end(); ++it) {
    if (it.key() != kPlatformPc && it.key() != kPlatformQuest) continue;
    PlatformEntry entry;
    const std::string reason = ReadPlatformEntry(it.value(), id, it.key(), &entry);
    if (!reason.empty()) return reason;
    m->platforms.emplace(it.key(), std::move(entry));
  }
  return "";
}

// A list of ids or of sha256 values. Absent is empty.
std::string ReadIdOrHashList(const json& doc, const char* key, bool ids, std::vector<std::string>* out) {
  const json* list = Find(doc, key);
  if (list == nullptr) return "";
  if (!list->is_array()) return "malformed";
  for (const json& item : *list) {
    const std::string* text = item.get_ptr<const std::string*>();
    if (text == nullptr) return "malformed";
    if (ids && !IsValidMapId(*text)) return "bad_id";
    if (!ids && !IsSha256Hex(*text)) return "bad_hash";
    out->push_back(*text);
  }
  return "";
}

// The floors a manifest meets, across the keys trusted now: the highest serial, with every body seen at it (one, unless
// a key was swapped in between), and the highest version of each entry.
struct TrustedFloor {
  std::uint64_t serial = 0;
  std::set<std::string> bodies;  // empty: no serial floor
  std::map<std::string, std::string> version;
};

TrustedFloor FloorOfTrustedKeys(const FloorBook& floors, const std::string& channel, const std::string& guild,
                                const std::set<std::string>& trusted_ids, std::vector<std::string>* untrusted) {
  TrustedFloor out;
  const auto by_guild = floors.channels.find(channel);
  if (by_guild == floors.channels.end()) return out;
  const auto by_key = by_guild->second.find(guild);
  if (by_key == by_guild->second.end()) return out;
  for (const auto& [key_id, floor] : by_key->second) {
    if (trusted_ids.count(key_id) == 0) {
      untrusted->push_back(key_id);
      continue;
    }
    if (!floor.serial_sha256.empty()) {
      if (out.bodies.empty() || floor.serial > out.serial) {
        out.serial = floor.serial;
        out.bodies.clear();
      }
      if (floor.serial == out.serial) out.bodies.insert(floor.serial_sha256);
    }
    for (const auto& [entry, version] : floor.version) {
      std::string& have = out.version[entry];
      if (have.empty() || CompareVersions(version, have) > 0) have = version;
    }
  }
  return out;
}

// The body is already authenticated; this is the shape check and the floors.
void ReadBody(std::string_view body, const FloorBook& floors, const std::set<std::string>& trusted_ids,
              const std::string& expected_channel, ParsedManifest* out) {
  json doc;
  if (!ParseStrict(body, &doc)) {
    out->reason = "malformed";
    return;
  }
  const std::string* schema = StringAt(doc, "schema");
  std::uint64_t schema_version = 0;
  if (schema == nullptr || *schema != kManifestSchema || !UnsignedAt(doc, "schema_version", &schema_version) ||
      schema_version != kManifestSchemaVersion) {
    out->reason = "bad_schema";
    return;
  }
  const json* channel_value = Find(doc, "channel");
  if (channel_value == nullptr) {
    out->reason = "channel_missing";
    return;
  }
  const std::string* channel = channel_value->get_ptr<const std::string*>();
  if (channel == nullptr || !IsValidChannel(*channel)) {
    out->reason = "bad_channel";
    return;
  }
  if (!expected_channel.empty() && *channel != expected_channel) {
    out->reason = "wrong_channel";
    return;
  }
  // The guild this manifest is for, inside the signed body, so a guild's manifest cannot be served as another's.
  const json* guild_value = Find(doc, "guild");
  if (guild_value == nullptr) {
    out->reason = "guild_missing";
    return;
  }
  const std::string* guild = guild_value->get_ptr<const std::string*>();
  if (guild == nullptr || !IsPathSafeGroupId(*guild)) {
    out->reason = "bad_guild";
    return;
  }
  Manifest& manifest = out->manifest;
  const std::string* expires = StringAt(doc, "expires");
  if (!UnsignedAt(doc, "serial", &manifest.serial) || manifest.serial == 0 || expires == nullptr || !IsValidUtc(*expires)) {
    out->reason = "malformed";
    return;
  }
  manifest.expires = *expires;

  const TrustedFloor floor = FloorOfTrustedKeys(floors, *channel, *guild, trusted_ids, &out->untrusted_floor_keys);
  if (!floor.bodies.empty()) {
    if (manifest.serial < floor.serial) {
      out->reason = "serial_down";
      return;
    }
    if (manifest.serial == floor.serial && floor.bodies.count(out->body_sha256) == 0) {
      out->reason = "serial_reuse";
      return;
    }
  }

  std::string reason = ReadIdOrHashList(doc, "revoked_ids", true, &manifest.revoked_ids);
  if (reason.empty()) reason = ReadIdOrHashList(doc, "revoked_sha256", false, &manifest.revoked_sha256);
  const json* maps = Find(doc, "maps");
  if (reason.empty() && (maps == nullptr || !maps->is_object())) reason = "malformed";
  if (!reason.empty()) {
    out->reason = reason;
    return;
  }
  for (auto it = maps->begin(); it != maps->end(); ++it) {
    if (!IsValidMapId(it.key())) {
      out->reason = "bad_id";
      return;
    }
    MapEntry entry;
    reason = ReadMapEntry(it.value(), it.key(), &entry);
    if (!reason.empty()) {
      out->reason = reason;
      return;
    }
    manifest.maps.emplace(it.key(), std::move(entry));
  }

  // Per entry: a rollback of one map drops that entry and nothing else, so revocations in the same body still apply.
  for (auto map = manifest.maps.begin(); map != manifest.maps.end(); ++map) {
    auto& platforms = map->second.platforms;
    for (auto p = platforms.begin(); p != platforms.end();) {
      const std::string key = map->first + "/" + p->first;
      const auto have = floor.version.find(key);
      if (have != floor.version.end() && CompareVersions(p->second.version, have->second) < 0) {
        out->dropped.emplace_back(key, "version_down");
        p = platforms.erase(p);
      } else {
        ++p;
      }
    }
  }
  for (auto map = manifest.maps.begin(); map != manifest.maps.end();) {
    map = map->second.platforms.empty() ? manifest.maps.erase(map) : std::next(map);
  }
  out->channel = *channel;
  out->guild = *guild;
}

bool AddWithin(std::uint64_t* total, std::uint64_t add, std::uint64_t limit) {
  if (add > limit || *total > limit - add) return false;
  *total += add;
  return true;
}

std::string ReadMember(const json& v, const std::string& platform, Member* m) {
  if (!v.is_object()) return "bad_member";
  const std::string* path = StringAt(v, "path");
  const std::string* role = StringAt(v, "role");
  const std::string* sha = StringAt(v, "sha256");
  if (path == nullptr || role == nullptr || sha == nullptr || !UnsignedAt(v, "size", &m->size)) return "bad_member";
  if (!IsSha256Hex(*sha)) return "bad_hash";
  m->path = *path;
  m->sha256 = *sha;
  std::string expected;
  if (*role == "set") {
    m->role = Role::kSet;
    expected = std::string(kSetPathPrefix) + platform + kSetPathSuffix;
  } else if (*role == "script") {
    m->role = Role::kScript;
    if (!Hex64At(v, "hash", &m->script_hash)) return "bad_member";
    expected = platform == kPlatformQuest
                   ? std::string(kScriptPathPrefix) + kQuestScriptPathPrefix + Hash16(m->script_hash) + kQuestScriptPathSuffix
                   : std::string(kScriptPathPrefix) + Hash16(m->script_hash) + kPcScriptPathSuffix;
  } else if (*role == "recipe") {
    m->role = Role::kRecipe;
    expected = std::string(kRecipePathPrefix) + platform + kRecipePathSuffix;
  } else {
    return "bad_member";
  }
  if (m->path != expected) return "bad_path";

  const json* pieces = Find(v, "pieces");
  if (pieces == nullptr || !pieces->is_array() || pieces->empty()) return "bad_pieces";
  std::uint64_t total = 0;
  for (const json& item : *pieces) {
    Pin piece;
    const std::string reason = ReadPin(&item, kMaxPieceBytes, &piece);
    if (reason == "bad_hash") return reason;
    if (!reason.empty() || !AddWithin(&total, piece.size, m->size)) return "bad_pieces";
    m->pieces.push_back(std::move(piece));
  }
  if (total != m->size) return "bad_pieces";
  return "";
}

std::string ReadRekeyed(const json& v, RekeyPin* r) {
  if (!v.is_object()) return "malformed";
  const std::string* stock_sha = StringAt(v, "stock_sha256");
  if (!Hex64At(v, "hash", &r->hash) || !Hex64At(v, "stock_source", &r->stock_source) || stock_sha == nullptr) {
    return "malformed";
  }
  if (!IsSha256Hex(*stock_sha)) return "bad_hash";
  r->stock_sha256 = *stock_sha;
  const json* edits = Find(v, "edits");
  if (edits == nullptr) return "";
  if (!edits->is_array()) return "rekey_edit";
  if (edits->empty()) return "";
  if (edits->size() > kRecipeOpsMax) return "recipe_ops";
  const std::string* result_sha = StringAt(v, "result_sha256");
  if (result_sha == nullptr || !UnsignedAt(v, "result_size", &r->result_size)) return "malformed";
  if (!IsSha256Hex(*result_sha)) return "bad_hash";
  r->result_sha256 = *result_sha;
  if (r->result_size > kRecipeResultMax) return "recipe_cap";
  if (r->result_size < kStockBytesMinSize) return "recipe_tiny";
  std::uint64_t next_free = 0;
  for (const json& item : *edits) {
    FieldEdit e;
    std::uint64_t width = 0;
    if (!item.is_object()) return "rekey_edit";
    if (Find(item, "from") != nullptr) return "recipe_from_forbidden";
    if (!UnsignedAt(item, "at", &e.at) || !UnsignedAt(item, "width", &width) || !Hex64At(item, "to", &e.to)) {
      return "rekey_edit";
    }
    // Each edit is the immediate of a 10-byte `mov r64, imm64`: sorted, and no site overlaps the one before.
    if (width != kRekeyEditWidth || e.at < kRekeyOpcodeBytes || e.at - kRekeyOpcodeBytes < next_free) return "rekey_edit";
    if (e.at > r->result_size || r->result_size - e.at < kRekeyEditWidth) return "recipe_bounds";
    e.width = static_cast<std::uint8_t>(kRekeyEditWidth);
    next_free = e.at + kRekeyEditWidth;
    r->edits.push_back(e);
  }
  return "";
}

// Every byte a recipe carries is authored: a field's width counts as INSERT bytes, like an insert range.
std::string ReadFieldOps(const json& ops, Recipe* r, std::uint64_t* carried) {
  std::uint64_t next_free = 0;
  for (const json& item : ops) {
    FieldEdit e;
    std::uint64_t width = 0;
    if (!item.is_object()) return "recipe_ops";
    if (Find(item, "from") != nullptr) return "recipe_from_forbidden";
    if (!UnsignedAt(item, "at", &e.at) || !UnsignedAt(item, "width", &width) || !Hex64At(item, "to", &e.to)) {
      return "recipe_ops";
    }
    if (std::find(std::begin(kFieldWidths), std::end(kFieldWidths), width) == std::end(kFieldWidths)) return "recipe_ops";
    if (width < sizeof(e.to) && (e.to >> (nevr_cargo::kBitsPerByte * width)) != 0) return "recipe_ops";
    if (e.at < next_free) return "recipe_ops";  // sorted and non-overlapping
    if (e.at > r->result_size || r->result_size - e.at < width) return "recipe_bounds";
    e.width = static_cast<std::uint8_t>(width);
    next_free = e.at + width;
    *carried += width;
    r->fields.push_back(e);
  }
  return "";
}

std::string ReadDiffOps(const json& ops, std::uint64_t recipe_member_size, bool has_recipe_member, Recipe* r,
                        std::uint64_t* carried) {
  std::uint64_t produced = 0;
  for (const json& item : ops) {
    if (!item.is_object() || item.size() != 1) return "recipe_ops";
    const bool insert = item.begin().key() == "insert";
    if (!insert && item.begin().key() != "copy") return "recipe_ops";
    const json& range = item.begin().value();
    constexpr std::size_t kRangeOffset = 0, kRangeSize = 1, kRangeItems = 2;  // [offset, size]
    const json::number_unsigned_t* offset = range.is_array() && range.size() == kRangeItems
                                                ? range[kRangeOffset].get_ptr<const json::number_unsigned_t*>()
                                                : nullptr;
    const json::number_unsigned_t* size =
        offset != nullptr ? range[kRangeSize].get_ptr<const json::number_unsigned_t*>() : nullptr;
    if (size == nullptr || *size == 0) return "recipe_ops";
    const std::uint64_t source_size = insert ? recipe_member_size : r->stock_size;
    if (insert && !has_recipe_member) return "bad_recipe_member";
    if (*offset > source_size || source_size - *offset < *size) return "recipe_bounds";
    if (!AddWithin(&produced, *size, r->result_size)) return "recipe_bounds";
    if (insert) *carried += *size;
    DiffOp op;
    op.insert = insert;
    op.offset = *offset;
    op.size = *size;
    r->diff.push_back(op);
  }
  if (produced != r->result_size) return "recipe_bounds";
  return "";
}

std::string ReadRecipe(const json& v, std::uint64_t recipe_member_size, bool has_recipe_member, Recipe* r,
                       std::uint64_t* map_carried, bool* inserts) {
  if (!v.is_object()) return "malformed";
  const json* stock = Find(v, "stock");
  const json* result = Find(v, "result");
  const json* edits = Find(v, "edits");
  if (!Hex64At(v, "type", &r->type) || !Hex64At(v, "name", &r->name) || stock == nullptr || !stock->is_object() ||
      result == nullptr || !result->is_object() || edits == nullptr || !edits->is_object()) {
    return "malformed";
  }
  const std::string* stock_sha = StringAt(*stock, "sha256");
  const std::string* result_sha = StringAt(*result, "sha256");
  if (!Hex64At(*stock, "type", &r->stock_type) || !Hex64At(*stock, "name", &r->stock_name) || stock_sha == nullptr ||
      !UnsignedAt(*stock, "size", &r->stock_size) || result_sha == nullptr || !UnsignedAt(*result, "size", &r->result_size)) {
    return "malformed";
  }
  if (!IsSha256Hex(*stock_sha) || !IsSha256Hex(*result_sha)) return "bad_hash";
  r->stock_sha256 = *stock_sha;
  r->result_sha256 = *result_sha;
  if (r->type == r->stock_type && r->name == r->stock_name) return "derived_stock_key";
  if (r->result_size > kRecipeResultMax) return "recipe_cap";
  if (r->result_size < kStockBytesMinSize) return "recipe_tiny";
  if (r->stock_size == 0) return "recipe_bounds";

  const std::string* kind = StringAt(*edits, "kind");
  const json* ops = Find(*edits, "ops");
  if (kind == nullptr || ops == nullptr || !ops->is_array()) return "malformed";
  if (ops->size() > kRecipeOpsMax) return "recipe_ops";
  std::uint64_t carried = 0;
  std::string reason;
  if (*kind == "fields") {
    r->kind = EditKind::kFields;
    if (r->result_size != r->stock_size) return "recipe_bounds";
    // No ops is the unchanged duplicate: legal only when it claims to rebuild the stock bytes exactly.
    if (ops->empty() && r->result_sha256 != r->stock_sha256) return "recipe_bounds";
    reason = ReadFieldOps(*ops, r, &carried);
  } else if (*kind == "diff") {
    r->kind = EditKind::kDiff;
    reason = ReadDiffOps(*ops, recipe_member_size, has_recipe_member, r, &carried);
    *inserts = *inserts || std::any_of(r->diff.begin(), r->diff.end(), [](const DiffOp& op) { return op.insert; });
  } else {
    return "malformed";
  }
  if (!reason.empty()) return reason;
  if (carried > std::min(r->result_size / kRecipeInsertDivisor, kRecipeInsertMax)) return "recipe_cap";
  if (!AddWithin(map_carried, carried, kRecipeMapInsertMax)) return "recipe_cap";
  return "";
}

std::string ReadAlias(const json& v, const std::string& id, ConfigAlias* a) {
  if (!v.is_object()) return "bad_alias";
  const std::string* key_name = StringAt(v, "key_name");
  const std::string* alias_name = StringAt(v, "alias_name");
  if (key_name == nullptr || alias_name == nullptr || !Hex64At(v, "key", &a->key) || !Hex64At(v, "alias", &a->alias)) {
    return "bad_alias";
  }
  const std::string prefix = id + "_";
  if (!IsSymbolName(*key_name) || !IsSymbolName(*alias_name) || alias_name->size() <= prefix.size() ||
      alias_name->compare(0, prefix.size(), prefix) != 0) {
    return "bad_alias";
  }
  if (a->key == a->alias || a->key != SymbolOf(*key_name) || a->alias != SymbolOf(*alias_name)) return "bad_alias";
  a->key_name = *key_name;
  a->alias_name = *alias_name;
  return "";
}

std::string ReadPackage(const json& doc, const std::string& id, const std::string& platform, const PlatformEntry& entry,
                        Package* p) {
  const std::string* schema = StringAt(doc, "schema");
  std::uint64_t schema_version = 0;
  if (schema == nullptr || *schema != kPackageSchema || !UnsignedAt(doc, "schema_version", &schema_version) ||
      schema_version != kPackageSchemaVersion) {
    return "bad_schema";
  }
  const std::string* doc_id = StringAt(doc, "id");
  const std::string* version = StringAt(doc, "version");
  const std::string* doc_platform = StringAt(doc, "platform");
  const std::string* level_name = StringAt(doc, "level_name");
  const std::string* game_build = StringAt(doc, "game_build");
  if (doc_id == nullptr || version == nullptr || doc_platform == nullptr || level_name == nullptr ||
      game_build == nullptr || !Hex64At(doc, "level", &p->level) || !Hex64At(doc, "parent", &p->parent)) {
    return "malformed";
  }
  if (*doc_id != id || *version != entry.version || *doc_platform != platform || p->level != entry.level ||
      *level_name != entry.level_name) {
    return "entry_mismatch";
  }
  p->id = *doc_id;
  p->version = *version;
  p->platform = *doc_platform;
  p->level_name = *level_name;
  p->game_build = *game_build;

  const json* source = Find(doc, "source_sha256");
  if (source == nullptr) return "source_missing";
  const std::string* source_sha = source->get_ptr<const std::string*>();
  if (source_sha == nullptr || !IsSha256Hex(*source_sha)) return "bad_hash";
  p->source_sha256 = *source_sha;
  if (const json* data_version = Find(doc, "stock_data_version")) {
    const std::string* text = data_version->get_ptr<const std::string*>();
    if (text == nullptr) return "malformed";
    p->stock_data_version = *text;
  }
  std::string reason = ReadIdOrHashList(doc, "requires_stock_manifest_sha256", false, &p->requires_stock_manifest_sha256);
  if (!reason.empty()) return reason;

  const json* members = Find(doc, "members");
  if (members == nullptr || !members->is_array() || members->empty()) return "bad_member";
  std::size_t sets = 0, recipes = 0;
  std::uint64_t recipe_member_size = 0;
  std::set<std::uint64_t> script_hashes;
  for (const json& item : *members) {
    Member m;
    reason = ReadMember(item, platform, &m);
    if (!reason.empty()) return reason;
    if (m.role == Role::kSet) ++sets;
    if (m.role == Role::kRecipe) {
      ++recipes;
      recipe_member_size = m.size;
    }
    if (m.role == Role::kScript && !script_hashes.insert(m.script_hash).second) return "dup_script";
    p->members.push_back(std::move(m));
  }
  if (sets != 1) return "set_count";
  if (recipes > 1) return "bad_recipe_member";
  if (recipe_member_size > kRecipeMapInsertMax) return "recipe_cap";

  if (const json* rekeyed = Find(doc, "rekeyed")) {
    if (!rekeyed->is_array()) return "malformed";
    for (const json& item : *rekeyed) {
      RekeyPin r;
      reason = ReadRekeyed(item, &r);
      if (!reason.empty()) return reason;
      if (!script_hashes.insert(r.hash).second) return "dup_script";
      p->rekeyed.push_back(std::move(r));
    }
  }

  if (const json* derived = Find(doc, "derived")) {
    if (!derived->is_array()) return "malformed";
    std::uint64_t map_carried = 0;
    bool inserts = false;
    std::set<std::pair<std::uint64_t, std::uint64_t>> targets;
    for (const json& item : *derived) {
      Recipe r;
      reason = ReadRecipe(item, recipe_member_size, recipes == 1, &r, &map_carried, &inserts);
      if (!reason.empty()) return reason;
      if (!targets.emplace(r.type, r.name).second) return "derived_dup";
      p->derived.push_back(std::move(r));
    }
    if (recipes == 1 && !inserts) return "bad_recipe_member";
  } else if (recipes == 1) {
    return "bad_recipe_member";
  }

  if (const json* standins = Find(doc, "standins")) {
    if (!standins->is_array()) return "malformed";
    for (const json& item : *standins) {
      const std::string* text = item.get_ptr<const std::string*>();
      std::uint64_t name = 0;
      if (text == nullptr || !ParseHex64(*text, &name)) return "malformed";
      p->standins.push_back(name);
    }
  }

  if (const json* aliases = Find(doc, "config_aliases")) {
    if (!aliases->is_array()) return "malformed";
    if (aliases->size() > kMaxConfigAliases) return "alias_count";
    std::set<std::uint64_t> keys;
    for (const json& item : *aliases) {
      ConfigAlias a;
      reason = ReadAlias(item, id, &a);
      if (!reason.empty()) return reason;
      if (!keys.insert(a.key).second) return "bad_alias";
      p->config_aliases.push_back(std::move(a));
    }
  }
  return "";
}

}  // namespace

std::string KeyIdText(const std::array<std::uint8_t, kMinisignKeyIdBytes>& key_id) {
  static const char kUpperDigits[] = "0123456789ABCDEF";
  std::string out;
  for (std::size_t i = kMinisignKeyIdBytes; i-- > 0;) {  // minisign prints the id as a little-endian number
    out.push_back(kUpperDigits[key_id[i] >> nevr_cargo::kHexDigitBits]);
    out.push_back(kUpperDigits[key_id[i] & nevr_cargo::kHexDigitMask]);
  }
  return out;
}

std::uint64_t MapDocumentBytes(const nevr_cargo::DeviceFacts& facts) noexcept {
  if (!facts.have_memory) return 0;
  return nevr_device::Share(facts.memory_available, nevr_cargo::kMemoryShareForManifest *
                                                        (nevr_cargo::kManifestBufferCopies + nevr_cargo::kJsonExpansion));
}

bool SplitSigned(std::string_view signed_object, std::string_view* minisig, std::string_view* body) {
  const std::size_t at = signed_object.find(kSignedSeparator);
  if (at == std::string_view::npos) return false;
  const std::string_view sig = signed_object.substr(0, at + 1);  // the minisig keeps its last line's newline
  const std::string_view rest = signed_object.substr(at + kSignedSeparator.size());
  if (rest.empty()) return false;
  *minisig = sig;
  *body = rest;
  return true;
}

ParsedManifest ParseAndVerifyManifest(std::string_view signed_object, const std::vector<PinnedKey>& keys,
                                      const FloorBook& floors, const std::string& expected_channel,
                                      const std::string& now_utc, std::uint64_t document_bytes) {
  ParsedManifest out;
  if (!IsValidUtc(now_utc)) {
    out.reason = "bad_now";
    return out;
  }
  if (document_bytes == 0) {
    out.reason = "no_memory";
    return out;
  }
  const std::uint64_t body_bytes = std::min(kMaxManifestBytes, document_bytes);
  if (signed_object.size() > kMaxMinisigBytes + kSignedSeparator.size() + body_bytes) {
    out.reason = "too_large";
    return out;
  }
  std::string_view minisig, body;
  if (!SplitSigned(signed_object, &minisig, &body)) {
    out.reason = "malformed";
    return out;
  }
  if (minisig.size() > kMaxMinisigBytes || body.size() > body_bytes) {
    out.reason = "too_large";
    return out;
  }

  // Official keys are tried apart from author keys, so `official` names the key that verified, never a shared key id.
  std::vector<nevr_minisign::PublicKey> official, author;
  for (const PinnedKey& k : keys) (k.official ? official : author).push_back(k.key);
  std::array<std::uint8_t, kMinisignKeyIdBytes> key_id{};
  nevr_minisign::Verdict verdict = nevr_minisign::Verify(body, minisig, official, &key_id, nullptr);
  out.official = verdict == nevr_minisign::Verdict::kOk;
  if (!out.official && verdict != nevr_minisign::Verdict::kMalformed && verdict != nevr_minisign::Verdict::kUnsupported) {
    const nevr_minisign::Verdict as_author = nevr_minisign::Verify(body, minisig, author, &key_id, nullptr);
    if (verdict == nevr_minisign::Verdict::kUnknownKey || as_author == nevr_minisign::Verdict::kOk) verdict = as_author;
  }
  if (verdict != nevr_minisign::Verdict::kOk) {
    out.reason = SignatureReason(verdict);
    return out;
  }
  out.key_id = KeyIdText(key_id);
  out.body_sha256 = nevr_quest::data::Sha256Hex(body.data(), body.size());
  std::set<std::string> trusted_ids;
  for (const PinnedKey& k : keys) trusted_ids.insert(KeyIdText(k.key.key_id));
  ReadBody(body, floors, trusted_ids, expected_channel, &out);
  if (!out.reason.empty()) {
    out.manifest = Manifest();
    out.dropped.clear();
    out.untrusted_floor_keys.clear();
    out.channel.clear();
    out.guild.clear();
  }
  return out;
}

FloorBook AdvanceFloors(const FloorBook& floors, const std::string& base_url, const ParsedManifest& parsed) {
  FloorBook next = floors;
  if (!parsed.reason.empty() || parsed.channel.empty() || parsed.guild.empty() || parsed.key_id.empty()) return next;
  Floors& floor = next.channels[parsed.channel][parsed.guild][parsed.key_id];
  if (floor.serial_sha256.empty() || parsed.manifest.serial >= floor.serial) {
    floor.serial = parsed.manifest.serial;
    floor.serial_sha256 = parsed.body_sha256;
  }
  for (const auto& map : parsed.manifest.maps) {
    for (const auto& platform : map.second.platforms) {
      std::string& version = floor.version[map.first + "/" + platform.first];
      if (version.empty() || CompareVersions(platform.second.version, version) > 0) version = platform.second.version;
    }
  }
  if (!base_url.empty()) next.bound[base_url] = parsed.channel;
  return next;
}

ParsedPackage ParsePackageIndex(std::string_view bytes, const std::string& id, const std::string& platform,
                                const PlatformEntry& entry, std::uint64_t document_bytes) {
  ParsedPackage out;
  if (document_bytes == 0) {
    out.reason = "no_memory";
    return out;
  }
  if (bytes.size() > std::min(kMaxIndexBytes, document_bytes)) {
    out.reason = "too_large";
    return out;
  }
  if (bytes.size() != entry.index.size || nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()) != entry.index.sha256) {
    out.reason = "index_pin";
    return out;
  }
  json doc;
  if (!ParseStrict(bytes, &doc)) {
    out.reason = "malformed";
    return out;
  }
  out.reason = ReadPackage(doc, id, platform, entry, &out.package);
  if (!out.reason.empty()) out.package = Package();
  return out;
}

int CompareVersions(std::string_view a, std::string_view b) {
  std::vector<std::uint32_t> pa, pb;
  const bool valid_a = VersionParts(a, &pa);
  const bool valid_b = VersionParts(b, &pb);
  if (!valid_a || !valid_b) return int(valid_a) - int(valid_b);
  for (std::size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
    const std::uint32_t x = i < pa.size() ? pa[i] : 0;
    const std::uint32_t y = i < pb.size() ? pb[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

bool IsValidVersion(std::string_view version) {
  std::vector<std::uint32_t> parts;
  return VersionParts(version, &parts);
}

bool IsValidMapId(std::string_view id) {
  if (id.empty() || id.size() > kMaxMapIdChars || id[0] < 'a' || id[0] > 'z') return false;
  return std::all_of(id.begin(), id.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool IsValidChannel(std::string_view channel) {
  if (channel.empty() || channel.size() > kMaxChannelChars) return false;
  const auto lower_digit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
  if (!lower_digit(channel[0])) return false;
  return std::all_of(channel.begin(), channel.end(), [&](char c) { return lower_digit(c) || c == '.' || c == '-'; });
}

bool IsPathSafeGroupId(std::string_view group) {
  if (group.empty() || group.size() > kGroupIdMaxBytes) return false;
  return std::all_of(group.begin(), group.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool IsValidUtc(std::string_view t) {
  if (t.size() != kUtcChars || t[kUtcTAt] != 'T' || t[kUtcZAt] != 'Z') return false;
  for (const std::size_t at : kUtcDateDashes) {
    if (t[at] != '-') return false;
  }
  for (const std::size_t at : kUtcTimeColons) {
    if (t[at] != ':') return false;
  }
  const auto number = [&](std::size_t at, std::size_t digits, int* out) {
    int v = 0;
    for (std::size_t i = at; i < at + digits; ++i) {
      if (t[i] < '0' || t[i] > '9') return false;
      v = v * static_cast<int>(kDecimalBase) + (t[i] - '0');
    }
    *out = v;
    return true;
  };
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!number(kUtcYearAt, kUtcYearDigits, &year) || !number(kUtcMonthAt, kUtcFieldDigits, &month) ||
      !number(kUtcDayAt, kUtcFieldDigits, &day) || !number(kUtcHourAt, kUtcFieldDigits, &hour) ||
      !number(kUtcMinuteAt, kUtcFieldDigits, &minute) || !number(kUtcSecondAt, kUtcFieldDigits, &second)) {
    return false;
  }
  if (month < 1 || month > kMonthsPerYear || hour > kMaxHour || minute > kMaxMinute || second > kMaxSecond) return false;
  const bool leap = (year % kLeapEvery == 0 && year % kLeapCenturySkip != 0) || year % kLeapCenturyKeep == 0;
  const int days = kDaysInMonth[month - 1] + (month == kFebruary && leap ? 1 : 0);
  return day >= 1 && day <= days;
}

bool IsExpired(const std::string& expires, const std::string& now_utc) {
  if (!IsValidUtc(expires) || !IsValidUtc(now_utc)) return true;
  return now_utc >= expires;  // one fixed-width zero-padded form, so text order is time order
}

}  // namespace nevr_maps
