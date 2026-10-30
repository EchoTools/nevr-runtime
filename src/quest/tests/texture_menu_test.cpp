// Host test for the Texture Resolution control's pure half (#1012): the two settings documents rewritten in memory
// (src/quest/sentinel/texture_menu.cpp), the saved value read back, and the level <-> menu index mapping
// (src/quest/sentinel/texture_quality.h).
//
// The documents here are synthetic: they have the shape the game's own files have (the keys the rewrite looks at, a neighbouring
// row, a neighbouring page, a second platform), with invented names where the rewrite does not look. Nothing of Ready At Dawn's
// data is stored in this tree. The expected labels, the page name and the control's name are written out as literals, so a change
// to the constants in texture_quality.h fails this test rather than following it.
//
// Built and run by `just test-quest-hooks`.

#include <cstdio>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "quest/sentinel/texture_menu.h"
#include "quest/sentinel/texture_quality.h"
#include "quest/tests/test_check.h"

namespace {

namespace tm = nevr_quest::texture_menu;
namespace tq = nevr_quest::texture_quality;
using Json = nlohmann::json;

// A menu document: a DISPLAY page of other settings, the graphics page that holds the msaa row between two others, and a page
// of another platform's with a row of the same path that is not the one we rewrite (it has no `rows` list holding it).
const char* kMenu = R"json({
  "serialized": 1,
  "pages": [
    {"name": "DISPLAY", "rows": [
      {"path": "graphics|fullscreen", "name": "Fullscreen", "value": true, "type": "NAMED FLAG"}
    ]},
    {"name": "GRAPHICS (1/3)", "rows": [
      {"path": "graphics|qualitylevel", "name": "Graphics Quality", "value": 2, "type": "CONTROL"},
      {"path": "graphics|msaa", "name": "Anti-aliasing", "idx": 0, "type": "STRING ARRAY", "values": ["None", "2x", "4x"]},
      {"path": "graphics|temporalaa", "name": "Temporal AA", "value": true, "type": "NAMED FLAG"}
    ]}
  ]
})json";

// A settings document: the shape of the quest section with its settings and pages lists, a rift section and a voice section.
const char* kGameSettings = R"json({
  "rift": {"settings": [{"path": "game|experimentalthrowing", "value": 0, "operator": "remove"}]},
  "quest": {
    "settings": [
      {"path": "graphics|resolutionscale", "value": 1.0, "operator": "remove"},
      {"path": "graphics|msaa", "value": 1, "operator": "remove"},
      {"path": "graphics|sharpening", "value": 2.0, "operator": "remove"}
    ],
    "pages": ["ECHO COMBAT", "DISPLAY", "GRAPHICS (1/3)", "GRAPHICS (2/3)"]
  },
  "voiploudness": {"settings": [{"path": "game|voiploudnesslevel", "value": 0.0, "operator": "remove"}], "pages": ["VOICE LOUDNESS"]}
})json";

const Json* PageNamed(const Json& doc, const std::string& name) {
  for (const Json& page : doc["pages"]) {
    if (page["name"] == name) return &page;
  }
  return nullptr;
}

void TheMenuLevelsAreInTheMenusOrder() {
  QCHECK(tq::kMenuLevelCount == 6);
  const char* const labels[] = {"Very Low", "Low", "Medium", "High", "Very High", "Ultra"};
  for (int i = 0; i < 6; ++i) QCHECK(std::strcmp(tq::kMenuLabels[i], labels[i]) == 0);
  QCHECK(std::strcmp(tq::kMenuControlName, "Texture Resolution") == 0);
  QCHECK(std::strcmp(tq::kMenuPageName, "GRAPHICS") == 0);
  QCHECK(tq::kDefaultMenuIndex == 1);  // Low, the stock size
  const tq::Level levels[] = {tq::Level::kVeryLow, tq::Level::kLow, tq::Level::kMedium,
                              tq::Level::kHigh,    tq::Level::kVeryHigh, tq::Level::kUltra};
  for (int i = 0; i < 6; ++i) {
    QCHECK(tq::LevelForMenuIndex(i) == levels[i]);
    QCHECK(tq::MenuIndexForLevel(levels[i]) == i);
  }
  // A saved number outside the menu is clamped, never wrapped.
  QCHECK(tq::LevelForMenuIndex(-1) == tq::Level::kVeryLow);
  QCHECK(tq::LevelForMenuIndex(6) == tq::Level::kUltra);
  QCHECK(tq::LevelForMenuIndex(100000) == tq::Level::kUltra);
  // `stock` has no entry; the menu shows it as Low.
  QCHECK(tq::MenuIndexForLevel(tq::Level::kStock) == 1);
}

void StockAndLowGiveTheSameEyeSizeOnEachHeadset() {
  for (const tq::EyeSize native : {tq::EyeSize{1680, 1760}, tq::EyeSize{1440, 1584}}) {
    const tq::EyeSize stock = tq::EffectiveSize(tq::Level::kStock, native);
    const tq::EyeSize low = tq::EffectiveSize(tq::Level::kLow, native);
    QCHECK(stock.width == native.width && stock.height == native.height);
    QCHECK(low.width == native.width && low.height == native.height);
  }
  // A level above Low is the table's, by the headset the native size names.
  const tq::EyeSize q3 = tq::EffectiveSize(tq::Level::kHigh, tq::EyeSize{1680, 1760});
  QCHECK(q3.width == 3072 && q3.height == 3380);
  const tq::EyeSize q2 = tq::EffectiveSize(tq::Level::kHigh, tq::EyeSize{1440, 1584});
  QCHECK(q2.width == 2016 && q2.height == 2224);
}

void TheMenuRowBecomesTheSixLevelControlOnAPageOfItsOwn() {
  const tm::Result r = tm::RewriteMenuSettings(kMenu, 1);
  QCHECK(r.status == tm::Status::kRewritten);
  QCHECK(r.rows == 1 && r.pages == 1);
  QCHECK(r.page == "GRAPHICS (1/3)");
  const Json doc = Json::parse(r.text, nullptr, false);
  QCHECK(!doc.is_discarded());
  // The page keeps the one row and is called GRAPHICS; the page it was is gone.
  QCHECK(PageNamed(doc, "GRAPHICS (1/3)") == nullptr);
  const Json* page = PageNamed(doc, "GRAPHICS");
  QCHECK(page != nullptr);
  if (page == nullptr) return;
  QCHECK((*page)["rows"].size() == 1);
  const Json& row = (*page)["rows"][0];
  QCHECK(row["path"] == "graphics|msaa");
  QCHECK(row["name"] == "Texture Resolution");
  QCHECK(row["type"] == "STRING ARRAY");
  QCHECK(row["idx"] == 1);
  QCHECK(row["values"] == Json::array({"Very Low", "Low", "Medium", "High", "Very High", "Ultra"}));
  // Another page is untouched.
  const Json* display = PageNamed(doc, "DISPLAY");
  QCHECK(display != nullptr && (*display)["rows"].size() == 1 && (*display)["rows"][0]["name"] == "Fullscreen");
  QCHECK(doc["serialized"] == 1);
}

void TheKeysKeepTheirOrderSoTheGameReadsWhatItReadBefore() {
  const tm::Result r = tm::RewriteMenuSettings(kMenu, 1);
  QCHECK(r.status == tm::Status::kRewritten);
  QCHECK(r.text.find("\"path\":\"graphics|msaa\",\"name\":\"Texture Resolution\",\"idx\":1,\"type\":\"STRING ARRAY\","
                     "\"values\":[\"Very Low\",\"Low\",\"Medium\",\"High\",\"Very High\",\"Ultra\"]") != std::string::npos);
  QCHECK(r.text.find('\n') == std::string::npos);  // one line: the game parses it as it parsed the file
}

void TheDefaultIndexIsTheCallers() {
  for (int index = 0; index < 6; ++index) {
    const tm::Result r = tm::RewriteMenuSettings(kMenu, index);
    QCHECK(r.status == tm::Status::kRewritten);
    const Json doc = Json::parse(r.text, nullptr, false);
    QCHECK((*PageNamed(doc, "GRAPHICS"))["rows"][0]["idx"] == index);
  }
}

void AMenuWithoutTheRowIsLeftAlone() {
  const tm::Result noRow = tm::RewriteMenuSettings(R"({"pages": [{"name": "DISPLAY", "rows": [{"path": "graphics|fullscreen"}]}]})", 1);
  QCHECK(noRow.status == tm::Status::kShapeMismatch && noRow.text.empty() && noRow.rows == 0);
  const tm::Result notJson = tm::RewriteMenuSettings("{\"pages\": [", 1);
  QCHECK(notJson.status == tm::Status::kNotJson && notJson.text.empty());
  QCHECK(tm::RewriteMenuSettings("", 1).status == tm::Status::kNotJson);
  QCHECK(tm::RewriteMenuSettings("[1, 2, 3]", 1).status == tm::Status::kShapeMismatch);
  // The path must be the whole string: a longer path is another setting.
  QCHECK(tm::RewriteMenuSettings(R"({"pages": [{"name": "P", "rows": [{"path": "graphics|msaa2"}]}]})", 1).status ==
         tm::Status::kShapeMismatch);
  QCHECK(std::strcmp(tm::StatusName(tm::Status::kRewritten), "rewritten") == 0);
  QCHECK(std::strcmp(tm::StatusName(tm::Status::kNotJson), "not_json") == 0);
  QCHECK(std::strcmp(tm::StatusName(tm::Status::kShapeMismatch), "shape_mismatch") == 0);
}

void TheSettingsDocumentStopsHidingTheControlAndItsPage() {
  const tm::Result r = tm::RewriteGameSettings(kGameSettings);
  QCHECK(r.status == tm::Status::kRewritten);
  QCHECK(r.rows == 1 && r.pages == 1);
  const Json doc = Json::parse(r.text, nullptr, false);
  QCHECK(!doc.is_discarded());
  // The quest list no longer removes graphics|msaa, and still removes the others.
  const Json& settings = doc["quest"]["settings"];
  QCHECK(settings.size() == 2);
  QCHECK(settings[0]["path"] == "graphics|resolutionscale" && settings[1]["path"] == "graphics|sharpening");
  // The quest page list no longer removes GRAPHICS (1/3), and still removes the others.
  QCHECK(doc["quest"]["pages"] == Json::array({"ECHO COMBAT", "DISPLAY", "GRAPHICS (2/3)"}));
  // The other sections are untouched.
  QCHECK(doc["rift"]["settings"].size() == 1 && doc["rift"]["settings"][0]["path"] == "game|experimentalthrowing");
  QCHECK(doc["voiploudness"]["settings"].size() == 1 && doc["voiploudness"]["pages"] == Json::array({"VOICE LOUDNESS"}));
}

void ASettingsDocumentWithoutTheRemovalIsLeftAlone() {
  const tm::Result r = tm::RewriteGameSettings(R"json({"quest": {"settings": [{"path": "graphics|sharpening"}], "pages": ["GRAPHICS (1/3)"]}})json");
  QCHECK(r.status == tm::Status::kShapeMismatch && r.text.empty());
  QCHECK(tm::RewriteGameSettings("not json").status == tm::Status::kNotJson);
  // A page removal alone is not the document we rewrite: the control would be listed but not shown.
  QCHECK(tm::RewriteGameSettings(R"json({"quest": {"pages": ["GRAPHICS (1/3)"]}})json").status == tm::Status::kShapeMismatch);
}

void TheRewriteDoesNotChangeItsOwnOutput() {
  // Reading the rewrite again finds nothing to do: the game never gets a rewrite of a rewrite.
  const tm::Result menu = tm::RewriteMenuSettings(kMenu, 1);
  QCHECK(menu.status == tm::Status::kRewritten);
  const tm::Result again = tm::RewriteMenuSettings(menu.text, 1);
  QCHECK(again.status == tm::Status::kRewritten && again.page == "GRAPHICS");  // it still has the row: it is idempotent
  QCHECK(Json::parse(again.text) == Json::parse(menu.text));
  const tm::Result settings = tm::RewriteGameSettings(kGameSettings);
  QCHECK(tm::RewriteGameSettings(settings.text).status == tm::Status::kShapeMismatch);
}

// ---- the player's marker ---------------------------------------------------------------------------------------------------

int Marked(const char* text) { return tm::MarkedMenuIndex(text, std::strlen(text)); }

void TheMarkedValueIsReadFromTheMarkerFile() {
  QCHECK(Marked(R"({"menu_index": 3})") == 3);
  QCHECK(Marked(R"({"menu_index":0})") == 0);
  QCHECK(Marked("{\"menu_index\" \t:\r\n 5 }") == 5);
  QCHECK(Marked(R"({"menu_index": 1, "other": 4})") == 1);
  QCHECK(Marked(R"({"menu_index": 1, "menu_index": 4})") == 1);  // the first one
  // The game's own slot is not the marker: a settings file that held the setting is not read as a choice.
  QCHECK(Marked(R"({"graphics": {"msaa": 4}})") == -1);
  QCHECK(Marked(R"({"graphics": {"msaa": 0}})") == -1);
}

void TheMarkerTextRoundTripsAndStaysInTheMenu() {
  for (int index = 0; index < 6; ++index) {
    const std::string text = tm::MarkerText(index);
    QCHECK(Json::parse(text) == Json::parse("{\"menu_index\":" + std::to_string(index) + "}"));  // valid JSON
    QCHECK(Marked(text.c_str()) == index);
  }
  QCHECK(Marked(tm::MarkerText(-4).c_str()) == 0);
  QCHECK(Marked(tm::MarkerText(99).c_str()) == 5);
}

void ANumberOutsideTheMenuIsClamped() {
  QCHECK(Marked(R"({"menu_index": 6})") == 5);
  QCHECK(Marked(R"({"menu_index": 99999999999999999999})") == 5);
  QCHECK(Marked(R"({"menu_index": -1})") == 0);
  QCHECK(Marked(R"({"menu_index": -300})") == 0);
}

void NothingUsableIsMinusOne() {
  QCHECK(Marked("") == -1);
  QCHECK(Marked("{}") == -1);
  QCHECK(Marked(R"({"menu_indexx": 3})") == -1);
  QCHECK(Marked(R"({"Menu_index": 3})") == -1);
  QCHECK(Marked(R"({"menu_index": "3"})") == -1);
  QCHECK(Marked(R"({"menu_index": null})") == -1);
  QCHECK(Marked(R"({"menu_index": true})") == -1);
  QCHECK(Marked(R"({"menu_index": 2.5})") == -1);
  QCHECK(Marked(R"({"menu_index": 2e1})") == -1);
  QCHECK(Marked(R"({"menu_index" 3})") == -1);
  QCHECK(tm::MarkedMenuIndex(nullptr, 10) == -1);
  // The size bounds the read: a number cut off by the end of the text is read as far as it goes, a key cut off is not a key.
  QCHECK(tm::MarkedMenuIndex("\"menu_index\":34", 14) == 3);
  QCHECK(tm::MarkedMenuIndex("\"menu_index\":34", 15) == 5);
  QCHECK(tm::MarkedMenuIndex("\"menu_index\":3", 13) == -1);
  QCHECK(tm::MarkedMenuIndex("\"menu_index\":34", 3) == -1);
}

}  // namespace

int main() {
  TheMenuLevelsAreInTheMenusOrder();
  StockAndLowGiveTheSameEyeSizeOnEachHeadset();
  TheMenuRowBecomesTheSixLevelControlOnAPageOfItsOwn();
  TheKeysKeepTheirOrderSoTheGameReadsWhatItReadBefore();
  TheDefaultIndexIsTheCallers();
  AMenuWithoutTheRowIsLeftAlone();
  TheSettingsDocumentStopsHidingTheControlAndItsPage();
  ASettingsDocumentWithoutTheRemovalIsLeftAlone();
  TheRewriteDoesNotChangeItsOwnOutput();
  TheMarkedValueIsReadFromTheMarkerFile();
  TheMarkerTextRoundTripsAndStaysInTheMenu();
  ANumberOutsideTheMenuIsClamped();
  NothingUsableIsMinusOne();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "texture_menu_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("texture_menu_test: all checks pass\n");
  return 0;
}
