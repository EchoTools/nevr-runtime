#include "texture_menu.h"

#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "quest/sentinel/hand_flip.h"
#include "quest/sentinel/texture_quality.h"

namespace nevr_quest::texture_menu {

namespace {

using Json = nlohmann::ordered_json;

bool HasPath(const Json& node, const char* wanted) {
  if (!node.is_object()) return false;
  const auto path = node.find("path");
  return path != node.end() && path->is_string() && path->get_ref<const std::string&>() == wanted;
}

bool IsMsaaRow(const Json& node) { return HasPath(node, kSettingPath); }

// KNOWN-WRONG #2043 The interim toggle's row: a NAMED FLAG, the kind the stock menu uses for this same boolean setting (its DISPLAY
// page's {"path":"graphics|fullscreen","name":"Fullscreen","value":true,"type":"NAMED FLAG"}), in that row's key order, with its two
// states named by `truestr` and `falsestr`. libr15 (ReVault VAs): CR15UISettingsCS::LoadSettingsFromJson (0x208ca4c) reads a NAMED
// FLAG row's `value` as a boolean (0x208e6b4), the state the row shows, and `truestr` and `falsestr` (0x208e7b8, 0x208f074; "" when
// absent, with no fallback label); the row's Serialize (0x21ebca8) writes the value back with CJson::SetBoolean. A STRING ARRAY row
// writes its `idx` with CJson::SetInt (Serialize 0x21ebb40), whose type check (0x10a605c) refuses an integer in a boolean key (headset
// log: "cannot change type of [graphics|fullscreen] to integer").
Json HandFlipRow(int handFlipIndex) {
  Json row = Json::object();
  row["path"] = nevr_quest::hand_flip::kMenuPath;
  row["name"] = nevr_quest::hand_flip::kMenuName;
  row["value"] = nevr_quest::hand_flip::MenuValueFor(handFlipIndex);
  row["type"] = "NAMED FLAG";
  row["truestr"] = nevr_quest::hand_flip::kMenuTrueLabel;
  row["falsestr"] = nevr_quest::hand_flip::kMenuFalseLabel;
  return row;
}

// Turns every `graphics|msaa` row into the control and every `rows` list that holds one into that row and the Controller Hand Flip
// row under it, under the control's page name. Preorder, as the rewrite of the patch repository walks: the page first, then its rows.
// KNOWN-WRONG #2043 `handFlipIndex` (the toggle's row index; kNoHandFlipRow: no row) is the interim toggle's.
void WalkMenu(Json& node, int defaultIndex, int handFlipIndex, Result* out) {
  if (node.is_object()) {
    if (IsMsaaRow(node)) {
      node["name"] = nevr_quest::texture_quality::kMenuControlName;
      node["type"] = "STRING ARRAY";
      Json labels = Json::array();
      for (const char* label : nevr_quest::texture_quality::kMenuLabels) labels.push_back(label);
      node["values"] = std::move(labels);
      node["idx"] = defaultIndex;
      ++out->rows;
    }
    const auto rows = node.find("rows");
    if (rows != node.end() && rows->is_array()) {
      bool holds = false;
      for (const Json& row : *rows) holds = holds || IsMsaaRow(row);
      if (holds) {
        Json kept = Json::array();
        bool flipAdded = false;  // KNOWN-WRONG #2043 the interim toggle's row, once per page
        for (Json& row : *rows) {
          if (!IsMsaaRow(row)) continue;
          if (!flipAdded && handFlipIndex != kNoHandFlipRow) {
            const Json flipRow = HandFlipRow(handFlipIndex);
            kept.push_back(std::move(row));
            kept.push_back(flipRow);
            flipAdded = true;
            ++out->handFlipRows;
          } else {
            kept.push_back(std::move(row));
          }
        }
        *rows = std::move(kept);
        const auto name = node.find("name");
        if (name != node.end() && name->is_string() && out->page.empty()) out->page = name->get<std::string>();
        node["name"] = nevr_quest::texture_quality::kMenuPageName;
        ++out->pages;
      }
    }
    for (auto& child : node) WalkMenu(child, defaultIndex, handFlipIndex, out);
  } else if (node.is_array()) {
    for (auto& child : node) WalkMenu(child, defaultIndex, handFlipIndex, out);
  }
}

// Removes the setting entries that remove `graphics|msaa` (and, KNOWN-WRONG #2043, the one that removes `graphics|fullscreen`, the
// interim toggle's setting), and the page entries that remove the stock graphics page, wherever a
// `settings` or `pages` list carries them.
void WalkGameSettings(Json& node, bool offerHandFlip, Result* out) {
  if (node.is_object()) {
    const auto settings = node.find("settings");
    if (settings != node.end() && settings->is_array()) {
      Json kept = Json::array();
      for (Json& entry : *settings) {
        if (IsMsaaRow(entry)) {
          ++out->rows;
        } else if (offerHandFlip && HasPath(entry, nevr_quest::hand_flip::kMenuPath)) {
          ++out->handFlipRows;
        } else {
          kept.push_back(std::move(entry));
        }
      }
      *settings = std::move(kept);
    }
    const auto pages = node.find("pages");
    if (pages != node.end() && pages->is_array()) {
      Json kept = Json::array();
      for (Json& entry : *pages) {
        if (entry.is_string() && entry.get_ref<const std::string&>() == kStockGraphicsPage) {
          ++out->pages;
        } else {
          kept.push_back(std::move(entry));
        }
      }
      *pages = std::move(kept);
    }
    for (auto& child : node) WalkGameSettings(child, offerHandFlip, out);
  } else if (node.is_array()) {
    for (auto& child : node) WalkGameSettings(child, offerHandFlip, out);
  }
}

bool Parse(std::string_view text, Json* out) {
  // sax_parse into a DOM, not json::parse: parse() instantiates the callback parser, whose std::function drags
  // std::bad_function_call's typeinfo into the .so as a default-visibility export (TestExportAllowlist). As sentinel_log.cpp does.
  using DomParser = nlohmann::detail::json_sax_dom_parser<Json, nlohmann::detail::iterator_input_adapter<std::string::const_iterator>>;
  const std::string input(text);
  DomParser dom(*out, /*allow_exceptions=*/false);
  if (Json::sax_parse(input, &dom)) return true;
  *out = Json();
  return false;
}

std::string Dump(const Json& doc) {
  return doc.dump(-1, ' ', /*ensure_ascii=*/false, Json::error_handler_t::replace);
}

}  // namespace

const char* StatusName(Status status) noexcept {
  switch (status) {
    case Status::kRewritten: return "rewritten";
    case Status::kNotJson: return "not_json";
    case Status::kShapeMismatch: return "shape_mismatch";
  }
  return "unknown";
}

Result RewriteMenuSettings(std::string_view text, int defaultIndex, int handFlipIndex) {
  Result result;
  Json doc;
  if (!Parse(text, &doc)) return result;
  WalkMenu(doc, defaultIndex, handFlipIndex, &result);
  if (result.rows == 0) {
    result.status = Status::kShapeMismatch;
    return result;
  }
  result.status = Status::kRewritten;
  result.text = Dump(doc);
  return result;
}

Result RewriteGameSettings(std::string_view text, bool offerHandFlip) {
  Result result;
  Json doc;
  if (!Parse(text, &doc)) return result;
  WalkGameSettings(doc, offerHandFlip, &result);
  if (result.rows == 0) {
    result.status = Status::kShapeMismatch;
    return result;
  }
  result.status = Status::kRewritten;
  result.text = Dump(doc);
  return result;
}

std::string MarkerText(int menuIndex) noexcept {
  const int last = nevr_quest::texture_quality::kMenuLevelCount - 1;
  Json doc = Json::object();
  doc["version"] = kMarkerVersion;
  doc["menu_index"] = menuIndex < 0 ? 0 : (menuIndex > last ? last : menuIndex);
  return Dump(doc);
}

namespace {

// The whole number after the first `key` (a quoted JSON key, with its quotes) in `text`: negative reads 0, a huge one saturates at
// 1000 (the caller clamps), and -1 means the key is absent, is cut off by `size`, or is not followed by a whole number.
// Allocation-free: it runs on the game's first eye-size query.
long WholeNumberAfterKey(const char* text, std::size_t size, const char* key, std::size_t keyLength) noexcept {
  if (text == nullptr) return -1;
  std::size_t at = 0;
  for (; at + keyLength <= size; ++at) {
    std::size_t i = 0;
    while (i < keyLength && text[at + i] == key[i]) ++i;
    if (i == keyLength) break;
  }
  if (at + keyLength > size) return -1;
  std::size_t i = at + keyLength;
  while (i < size && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
  if (i >= size || text[i] != ':') return -1;
  ++i;
  while (i < size && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
  bool negative = false;
  if (i < size && text[i] == '-') {
    negative = true;
    ++i;
  }
  if (i >= size || text[i] < '0' || text[i] > '9') return -1;
  long value = 0;
  while (i < size && text[i] >= '0' && text[i] <= '9') {
    if (value < 1000) value = value * 10 + (text[i] - '0');
    ++i;
  }
  if (i < size && (text[i] == '.' || text[i] == 'e' || text[i] == 'E')) return -1;  // not a whole number
  return negative ? 0 : value;
}

}  // namespace

int MarkedMenuIndex(const char* text, std::size_t size) noexcept {
  constexpr char kKey[] = "\"menu_index\"";
  const long value = WholeNumberAfterKey(text, size, kKey, sizeof(kKey) - 1);
  if (value < 0) return -1;
  const int last = nevr_quest::texture_quality::kMenuLevelCount - 1;
  return value > last ? last : static_cast<int>(value);
}

int MarkerVersion(const char* text, std::size_t size) noexcept {
  constexpr char kKey[] = "\"version\"";
  const long value = WholeNumberAfterKey(text, size, kKey, sizeof(kKey) - 1);
  return value < 0 ? 0 : static_cast<int>(value);
}

}  // namespace nevr_quest::texture_menu
