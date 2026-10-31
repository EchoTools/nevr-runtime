#include "quest/patch/libr15_extra_rows.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <set>

namespace nevr_quest::patch {
namespace {

using Json = nlohmann::json;

Refusal Refuse(const char* code, std::string id, std::string detail) { return Refusal{code, std::move(id), std::move(detail)}; }

}  // namespace

PatchSpec PublicSpec() {
  PatchSpec spec;
  spec.rows = kRows;
  spec.rowCount = kRowCount;
  ParseBuildIdHex(kLibR15StockBuildId, &spec.stockBuildId);
  spec.stockSha256 = kLibR15StockSha256;
  for (const char* sha : kLibR15ReferenceSha256) spec.alsoSha256.emplace_back(sha);
  spec.expectedImageEnd = kStockImageEnd;
  spec.slotAreaGap = kSlotAreaGap;
  return spec;
}

Json Refusal::ToJson() const {
  Json j = {{"event", "libr15_patch"}, {"status", "refused"}, {"reason", code}, {"detail", detail}};
  if (!id.empty()) j["id"] = id;
  return j;
}

namespace {

bool IsLowerHex(const std::string& s, std::size_t length) {
  if (s.size() != length) return false;
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool ParseBytes(const std::string& s, WordBytes* out) {
  if (!IsLowerHex(s, 8)) return false;
  for (std::size_t i = 0; i < 4; ++i) {
    (*out)[i] = static_cast<std::uint8_t>(std::stoul(s.substr(2 * i, 2), nullptr, 16));
  }
  return true;
}

// `0x` and 1 to 16 lowercase hex digits, no leading zero (the value zero is `0x0`).
bool ParseHexAddress(const std::string& s, std::uint64_t* out) {
  if (s.size() < 3 || s.size() > 18 || s[0] != '0' || s[1] != 'x') return false;
  const std::string digits = s.substr(2);
  if (!IsLowerHex(digits, digits.size())) return false;
  if (digits.size() > 1 && digits[0] == '0') return false;
  *out = std::stoull(digits, nullptr, 16);
  return true;
}

bool ValidExtraId(const std::string& id) {
  const std::size_t prefix = std::strlen(kExtraRowIdPrefix);
  if (id.size() <= prefix || id.size() > prefix + kExtraRowIdMaxSuffix) return false;
  if (id.compare(0, prefix, kExtraRowIdPrefix) != 0) return false;
  for (std::size_t i = prefix; i < id.size(); ++i) {
    const char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
  }
  return true;
}

// Every key of `object` must be one of `allowed`, and every allowed key present.
bool CheckKeys(const Json& object, std::initializer_list<const char*> allowed, const std::string& where, const std::string& id,
               Refusal* refusal) {
  for (auto it = object.begin(); it != object.end(); ++it) {
    bool known = false;
    for (const char* k : allowed) known = known || it.key() == k;
    if (!known) {
      *refusal = Refuse("extra_rows_unknown_field", id, where + ": unknown field \"" + it.key() + "\"");
      return false;
    }
  }
  for (const char* k : allowed) {
    if (!object.contains(k)) {
      *refusal = Refuse("extra_rows_missing_field", id, where + ": missing field \"" + k + "\"");
      return false;
    }
  }
  return true;
}

}  // namespace

bool ParseExtraRows(const std::string& fileBytes, const PatchSpec& spec, std::vector<WordPatchRow>* rows, Refusal* refusal) {
  rows->clear();
  // The parser reports every key as it reads it; an object that repeats one is a refusal, because the tree would
  // keep only the last value and the file would not mean what it says.
  std::vector<std::set<std::string>> keys;
  bool duplicate = false;
  std::string duplicateKey;
  const Json::parser_callback_t onEvent = [&](int, Json::parse_event_t event, Json& parsed) {
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    if (event == Json::parse_event_t::object_end && !keys.empty()) keys.pop_back();
    if (event == Json::parse_event_t::key && !keys.empty() && !keys.back().insert(parsed.get<std::string>()).second) {
      duplicate = true;
      duplicateKey = parsed.get<std::string>();
    }
    return true;
  };
  const Json doc = Json::parse(fileBytes, onEvent, /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    *refusal = Refuse("extra_rows_json", "", "the file is not valid UTF-8 JSON");
    return false;
  }
  if (duplicate) {
    *refusal = Refuse("extra_rows_duplicate_key", "", "an object repeats the key \"" + duplicateKey + "\"");
    return false;
  }
  if (!doc.is_object()) {
    *refusal = Refuse("extra_rows_json", "", "the top level is not an object");
    return false;
  }
  if (!CheckKeys(doc, {"format", "version", "stock_build_id", "rows"}, "file", "", refusal)) return false;
  if (!doc["format"].is_string() || doc["format"].get<std::string>() != kExtraRowsFormat) {
    *refusal = Refuse("extra_rows_format", "", std::string("format must be exactly \"") + kExtraRowsFormat + "\"");
    return false;
  }
  if (!doc["version"].is_number_integer() || doc["version"].get<std::int64_t>() != kExtraRowsVersion) {
    *refusal = Refuse("extra_rows_version", "", "version must be the integer 1");
    return false;
  }
  // BuildIdHex is 40 lowercase digits, so equality with it also refuses a wrong length or an upper-case digit.
  if (!doc["stock_build_id"].is_string() || doc["stock_build_id"].get<std::string>() != BuildIdHex(spec.stockBuildId)) {
    *refusal = Refuse("extra_rows_stock_build_id", "", "stock_build_id is not the pinned stock build ID " + BuildIdHex(spec.stockBuildId));
    return false;
  }
  if (!doc["rows"].is_array() || doc["rows"].empty()) {
    *refusal = Refuse("extra_rows_missing_field", "", "rows must be an array of at least one row");
    return false;
  }
  std::set<std::string> ids;
  std::size_t totalWords = 0;
  for (const Json& row : doc["rows"]) {
    if (!row.is_object()) {
      *refusal = Refuse("extra_rows_json", "", "a row is not an object");
      return false;
    }
    std::string id;
    if (row.contains("id") && row["id"].is_string()) id = row["id"].get<std::string>();
    if (!CheckKeys(row, {"id", "kind", "disasm_va", "words"}, "row", id, refusal)) return false;
    if (!ValidExtraId(id)) {
      *refusal = Refuse("extra_rows_id", id, "the id must be x. followed by 1 to 62 of [a-z0-9_.]: \"" + id + "\"");
      return false;
    }
    bool isPublic = false;
    for (std::size_t i = 0; i < spec.rowCount; ++i) isPublic = isPublic || id == spec.rows[i].id;
    if (isPublic || !ids.insert(id).second) {
      *refusal = Refuse("extra_rows_id", id, isPublic ? "the id is a public row's" : "the id is used twice in the file");
      return false;
    }
    if (!row["kind"].is_string() || row["kind"].get<std::string>() != "word_patch") {
      *refusal = Refuse("extra_rows_kind", id, "kind must be exactly \"word_patch\"");
      return false;
    }
    WordPatchRow out;
    out.id = id;
    if (!row["disasm_va"].is_string() || !ParseHexAddress(row["disasm_va"].get<std::string>(), &out.disasmVa) ||
        (out.disasmVa & 3) != 0 || out.disasmVa > ~std::uint64_t{0} - kMaxWordOffset - 4) {
      *refusal = Refuse("extra_rows_disasm_va", id, "disasm_va must be 0x lowercase hex without leading zeros, a multiple of 4");
      return false;
    }
    const Json& words = row["words"];
    if (!words.is_array() || words.empty() || words.size() > kMaxWordsPerRow) {
      *refusal = Refuse("extra_rows_words", id, "words must hold 1 to 64 entries");
      return false;
    }
    totalWords += words.size();
    if (totalWords > kMaxWordsPerFile) {
      *refusal = Refuse("extra_rows_words", id, "the file holds more than 256 words");
      return false;
    }
    std::int64_t previous = -1;
    for (const Json& w : words) {
      if (!w.is_object() || !CheckKeys(w, {"offset", "stock_bytes", "new_bytes"}, "word", id, refusal)) {
        if (!w.is_object()) *refusal = Refuse("extra_rows_json", id, "a word is not an object");
        return false;
      }
      const Json& off = w["offset"];
      // `previous` starts at -1, so "strictly increasing" also refuses every negative offset.
      if (!off.is_number_integer() || off.get<std::int64_t>() > kMaxWordOffset ||
          (off.get<std::int64_t>() & 3) != 0 || off.get<std::int64_t>() <= previous) {
        *refusal = Refuse("extra_rows_offset", id, "offset must be an integer multiple of 4 in 0 to 4092, strictly increasing");
        return false;
      }
      previous = off.get<std::int64_t>();
      WordEdit edit;
      edit.offset = static_cast<std::uint32_t>(previous);
      if (!w["stock_bytes"].is_string() || !ParseBytes(w["stock_bytes"].get<std::string>(), &edit.stock) ||
          !w["new_bytes"].is_string() || !ParseBytes(w["new_bytes"].get<std::string>(), &edit.word)) {
        *refusal = Refuse("extra_rows_bytes", id, "stock_bytes and new_bytes must each be exactly 8 lowercase hex digits (bytes in memory order)");
        return false;
      }
      if (edit.stock == edit.word) {
        *refusal = Refuse("extra_rows_unchanged", id, "new_bytes equals stock_bytes at offset " + std::to_string(edit.offset));
        return false;
      }
      out.edits.push_back(edit);
    }
    rows->push_back(std::move(out));
  }
  return true;
}
}  // namespace nevr_quest::patch
