#include "texture_menu.h"

#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "quest/sentinel/texture_quality.h"

namespace nevr_quest::texture_menu {

namespace {

using Json = nlohmann::ordered_json;

bool IsMsaaRow(const Json& node) {
  if (!node.is_object()) return false;
  const auto path = node.find("path");
  return path != node.end() && path->is_string() && path->get_ref<const std::string&>() == kSettingPath;
}

// Turns every `graphics|msaa` row into the control and every `rows` list that holds one into that row alone, under the control's
// page name. Preorder, as the rewrite of the patch repository walks: the page first, then its rows.
void WalkMenu(Json& node, int defaultIndex, Result* out) {
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
        for (Json& row : *rows) {
          if (IsMsaaRow(row)) kept.push_back(std::move(row));
        }
        *rows = std::move(kept);
        const auto name = node.find("name");
        if (name != node.end() && name->is_string() && out->page.empty()) out->page = name->get<std::string>();
        node["name"] = nevr_quest::texture_quality::kMenuPageName;
        ++out->pages;
      }
    }
    for (auto& child : node) WalkMenu(child, defaultIndex, out);
  } else if (node.is_array()) {
    for (auto& child : node) WalkMenu(child, defaultIndex, out);
  }
}

// Removes the setting entries that remove `graphics|msaa`, and the page entries that remove the stock graphics page, wherever a
// `settings` or `pages` list carries them.
void WalkGameSettings(Json& node, Result* out) {
  if (node.is_object()) {
    const auto settings = node.find("settings");
    if (settings != node.end() && settings->is_array()) {
      Json kept = Json::array();
      for (Json& entry : *settings) {
        if (IsMsaaRow(entry)) {
          ++out->rows;
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
    for (auto& child : node) WalkGameSettings(child, out);
  } else if (node.is_array()) {
    for (auto& child : node) WalkGameSettings(child, out);
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

Result RewriteMenuSettings(std::string_view text, int defaultIndex) {
  Result result;
  Json doc;
  if (!Parse(text, &doc)) return result;
  WalkMenu(doc, defaultIndex, &result);
  if (result.rows == 0) {
    result.status = Status::kShapeMismatch;
    return result;
  }
  result.status = Status::kRewritten;
  result.text = Dump(doc);
  return result;
}

Result RewriteGameSettings(std::string_view text) {
  Result result;
  Json doc;
  if (!Parse(text, &doc)) return result;
  WalkGameSettings(doc, &result);
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
  doc["menu_index"] = menuIndex < 0 ? 0 : (menuIndex > last ? last : menuIndex);
  return Dump(doc);
}

int MarkedMenuIndex(const char* text, std::size_t size) noexcept {
  if (text == nullptr) return -1;
  constexpr char kKey[] = "\"menu_index\"";
  constexpr std::size_t kKeyLength = sizeof(kKey) - 1;
  std::size_t at = 0;
  for (; at + kKeyLength <= size; ++at) {
    std::size_t i = 0;
    while (i < kKeyLength && text[at + i] == kKey[i]) ++i;
    if (i == kKeyLength) break;
  }
  if (at + kKeyLength > size) return -1;
  std::size_t i = at + kKeyLength;
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
  if (negative) value = 0;
  const int last = nevr_quest::texture_quality::kMenuLevelCount - 1;
  return value > last ? last : static_cast<int>(value);
}

}  // namespace nevr_quest::texture_menu
