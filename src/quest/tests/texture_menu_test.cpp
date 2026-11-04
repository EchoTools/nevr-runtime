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

#include "quest/headset_profile.h"
#include "quest/sentinel/texture_menu.h"
#include "quest/sentinel/texture_quality.h"
#include "quest/tests/test_check.h"

namespace {

namespace tm = nevr_quest::texture_menu;
namespace tq = nevr_quest::texture_quality;
namespace headset_profile = nevr_quest::headset_profile;
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
  QCHECK(std::strcmp(tq::kMenuPageName, "NEVR") == 0);
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
  const headset_profile::Profile* const profiles[] = {&headset_profile::kQuest3, &headset_profile::kQuest2};
  const tq::EyeSize natives[] = {tq::EyeSize{1680, 1760}, tq::EyeSize{1440, 1584}};
  for (int i = 0; i < 2; ++i) {
    const tq::EyeSize stock = tq::EffectiveSize(tq::Level::kStock, natives[i], profiles[i]);
    const tq::EyeSize low = tq::EffectiveSize(tq::Level::kLow, natives[i], profiles[i]);
    QCHECK(stock.width == natives[i].width && stock.height == natives[i].height);
    QCHECK(low.width == natives[i].width && low.height == natives[i].height);
  }
  // A level above Low is the table of the profile the identity step set, not a table picked by the native size.
  const tq::EyeSize q3 = tq::EffectiveSize(tq::Level::kHigh, tq::EyeSize{1680, 1760}, &headset_profile::kQuest3);
  QCHECK(q3.width == 3072 && q3.height == 3380);
  const tq::EyeSize q2 = tq::EffectiveSize(tq::Level::kHigh, tq::EyeSize{1440, 1584}, &headset_profile::kQuest2);
  QCHECK(q2.width == 2016 && q2.height == 2224);
  // The native size does not name the headset: a Quest 2 profile gives its table whatever the native size is, and with no
  // profile yet the native size stands.
  const tq::EyeSize byProfile = tq::EffectiveSize(tq::Level::kHigh, tq::EyeSize{1680, 1760}, &headset_profile::kQuest2);
  QCHECK(byProfile.width == 2016 && byProfile.height == 2224);
  const tq::EyeSize unknown = tq::EffectiveSize(tq::Level::kHigh, tq::EyeSize{1680, 1760}, nullptr);
  QCHECK(unknown.width == 1680 && unknown.height == 1760);
}

void TheMenuRowBecomesTheSixLevelControlOnAPageOfItsOwn() {
  const tm::Result r = tm::RewriteMenuSettings(kMenu, 1);
  QCHECK(r.status == tm::Status::kRewritten);
  QCHECK(r.rows == 1 && r.pages == 1 && r.handFlipRows == 1);
  QCHECK(r.page == "GRAPHICS (1/3)");
  const Json doc = Json::parse(r.text, nullptr, false);
  QCHECK(!doc.is_discarded());
  // The page keeps the Texture Resolution row and the Controller Hand Flip row under it, and is called NEVR; the page it was is
  // gone.
  QCHECK(PageNamed(doc, "GRAPHICS (1/3)") == nullptr);
  const Json* page = PageNamed(doc, "NEVR");
  QCHECK(page != nullptr);
  if (page == nullptr) return;
  QCHECK((*page)["rows"].size() == 2);
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
    QCHECK((*PageNamed(doc, "NEVR"))["rows"][0]["idx"] == index);
  }
}

// KNOWN-WRONG #2043 The interim Controller Hand Flip row: the second row of the page, after Texture Resolution, a NAMED FLAG bound
// to `graphics|fullscreen` whose true is "Off" (the bit set) and false is "On", starting on the choice the caller gives (index 1, Off,
// by default: index 0 is On).
void TheHandFlipRowIsTheSecondRowAfterTextureResolution() {
  const tm::Result r = tm::RewriteMenuSettings(kMenu, 1);  // no index given: the row starts on Off
  QCHECK(r.status == tm::Status::kRewritten && r.handFlipRows == 1);
  const Json doc = Json::parse(r.text, nullptr, false);
  const Json* page = PageNamed(doc, "NEVR");
  QCHECK(page != nullptr && (*page)["rows"].size() == 2);
  if (page == nullptr || (*page)["rows"].size() != 2) return;
  QCHECK((*page)["rows"][0]["name"] == "Texture Resolution");
  const Json& flip = (*page)["rows"][1];
  QCHECK(flip["path"] == "graphics|fullscreen");
  QCHECK(flip["name"] == "Controller Hand Flip");
  QCHECK(flip["type"] == "NAMED FLAG");
  QCHECK(flip["truestr"] == "Off" && flip["falsestr"] == "On");
  QCHECK(flip["value"] == true);  // the default is Off: the bit set, the game's own value
  QCHECK(!flip.contains("idx") && !flip.contains("values"));  // a STRING ARRAY's keys: the game would write an index back
  // The stock fullscreen row on the DISPLAY page is another page's and is not ours.
  const Json* display = PageNamed(doc, "DISPLAY");
  QCHECK(display != nullptr && (*display)["rows"].size() == 1 && (*display)["rows"][0]["type"] == "NAMED FLAG");
  // The caller's index is the row's start (the stored choice): 0 is On (false), anything else is Off (true).
  for (const int index : {0, 1, 5}) {
    const Json withIndex = Json::parse(tm::RewriteMenuSettings(kMenu, 1, index).text, nullptr, false);
    QCHECK((*PageNamed(withIndex, "NEVR"))["rows"][1]["value"] == (index != 0));
  }
}

// The texture rows are what they were: the Texture Resolution row is the same with and without the new row beside it.
void TheTextureRowIsUnchangedByTheNewRow() {
  const Json doc = Json::parse(tm::RewriteMenuSettings(kMenu, 3, 1).text, nullptr, false);
  const Json& row = (*PageNamed(doc, "NEVR"))["rows"][0];
  QCHECK(row["path"] == "graphics|msaa" && row["name"] == "Texture Resolution" && row["type"] == "STRING ARRAY");
  QCHECK(row["idx"] == 3);  // the texture default, not the hand flip index
  QCHECK(row["values"] == Json::array({"Very Low", "Low", "Medium", "High", "Very High", "Ultra"}));
  QCHECK(row.size() == 5);  // path, name, idx, type, values: the stock row's keys, no more
  // The row beside it has the stock NAMED FLAG row's keys, in its order, and the two state names: nothing the engine does not read.
  const Json& flip = (*PageNamed(doc, "NEVR"))["rows"][1];
  QCHECK(flip.size() == 6);
  // The order is read from the text: this file's Json sorts its keys, the rewrite keeps the order it wrote.
  const std::string text = tm::RewriteMenuSettings(kMenu, 3, 1).text;
  QCHECK(text.find(R"({"path":"graphics|fullscreen","name":"Controller Hand Flip","value":true,"type":"NAMED FLAG","truestr":"Off",)"
                   R"("falsestr":"On"})") != std::string::npos);
}

// A menu with no Texture Resolution row to hang it under is a shape mismatch: the row is added beside the texture control or not
// at all, never alone.
void TheHandFlipRowIsNeverAddedAlone() {
  const tm::Result alone = tm::RewriteMenuSettings(
      R"({"pages": [{"name": "DISPLAY", "rows": [{"path": "graphics|fullscreen", "name": "Fullscreen", "type": "NAMED FLAG"}]}]})", 1, 1);
  QCHECK(alone.status == tm::Status::kShapeMismatch && alone.text.empty() && alone.handFlipRows == 0);
}

// With no row offered (the hand pose hooks are not installed) the menu keeps the Texture Resolution row alone and the settings
// document keeps its stock entry for the toggle's setting.
void ANotOfferedRowAddsNoRowAndKeepsTheStockEntry() {
  const tm::Result r = tm::RewriteMenuSettings(kMenu, 1, tm::kNoHandFlipRow);
  QCHECK(r.status == tm::Status::kRewritten && r.rows == 1 && r.handFlipRows == 0);
  const Json doc = Json::parse(r.text, nullptr, false);
  QCHECK((*PageNamed(doc, "NEVR"))["rows"].size() == 1);
  QCHECK((*PageNamed(doc, "NEVR"))["rows"][0]["name"] == "Texture Resolution");
  const char* const document = R"json({"quest": {"settings": [{"path": "graphics|msaa", "operator": "remove"},
      {"path": "graphics|fullscreen", "value": true, "operator": "remove"}], "pages": ["GRAPHICS (1/3)"]}})json";
  const tm::Result settings = tm::RewriteGameSettings(document, false);
  QCHECK(settings.status == tm::Status::kRewritten && settings.rows == 1 && settings.handFlipRows == 0);
  const Json kept = Json::parse(settings.text, nullptr, false);
  QCHECK(kept["quest"]["settings"].size() == 1 && kept["quest"]["settings"][0]["path"] == "graphics|fullscreen");
}

// The hiding entry of the settings document goes with the control's: the row is shown and its value saved.
void TheSettingsDocumentStopsHidingTheHandFlipSetting() {
  const char* const document = R"json({"quest": {"settings": [{"path": "graphics|msaa", "operator": "remove"},
      {"path": "graphics|fullscreen", "value": true, "operator": "remove"}, {"path": "graphics|sharpening", "operator": "remove"}],
      "pages": ["GRAPHICS (1/3)"]}})json";
  const tm::Result r = tm::RewriteGameSettings(document);
  QCHECK(r.status == tm::Status::kRewritten && r.rows == 1 && r.handFlipRows == 1);
  const Json doc = Json::parse(r.text, nullptr, false);
  QCHECK(doc["quest"]["settings"].size() == 1 && doc["quest"]["settings"][0]["path"] == "graphics|sharpening");
  // A document with no msaa removal is still not ours, whatever else it hides.
  const tm::Result alone = tm::RewriteGameSettings(
      R"json({"quest": {"settings": [{"path": "graphics|fullscreen", "operator": "remove"}], "pages": ["GRAPHICS (1/3)"]}})json");
  QCHECK(alone.status == tm::Status::kShapeMismatch && alone.text.empty());
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
  QCHECK(again.status == tm::Status::kRewritten && again.page == "NEVR");  // it still has the row: it is idempotent
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
    QCHECK(Json::parse(text) == Json::parse("{\"version\":1,\"menu_index\":" + std::to_string(index) + "}"));  // valid JSON
    QCHECK(Marked(text.c_str()) == index);
    QCHECK(tm::MarkerVersion(text.c_str(), text.size()) == tm::kMarkerVersion);
  }
  QCHECK(Marked(tm::MarkerText(-4).c_str()) == 0);
  QCHECK(Marked(tm::MarkerText(99).c_str()) == 5);
}

int Version(const char* text) { return tm::MarkerVersion(text, std::strlen(text)); }

void TheMarkerVersionIsReadFromTheMarkerFile() {
  QCHECK(Version(R"({"version":1,"menu_index":3})") == 1);
  QCHECK(Version("{\"version\" \t:\r\n 7 }") == 7);
  // An unversioned marker (what 6.14 to 6.16 wrote) reads 0, as does anything that is not a whole number.
  QCHECK(Version(R"({"menu_index":3})") == 0);
  QCHECK(Version("") == 0);
  QCHECK(Version(R"({"version":"1"})") == 0);
  QCHECK(Version(R"({"version":1.5})") == 0);
  QCHECK(Version(R"({"version":-1})") == 0);
  QCHECK(Version(R"({"versions":1})") == 0);
  QCHECK(tm::MarkerVersion(nullptr, 10) == 0);
  QCHECK(tm::kMarkerVersion == 1);
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
  TheHandFlipRowIsTheSecondRowAfterTextureResolution();
  TheTextureRowIsUnchangedByTheNewRow();
  TheHandFlipRowIsNeverAddedAlone();
  TheSettingsDocumentStopsHidingTheHandFlipSetting();
  ANotOfferedRowAddsNoRowAndKeepsTheStockEntry();
  AMenuWithoutTheRowIsLeftAlone();
  TheSettingsDocumentStopsHidingTheControlAndItsPage();
  ASettingsDocumentWithoutTheRemovalIsLeftAlone();
  TheRewriteDoesNotChangeItsOwnOutput();
  TheMarkedValueIsReadFromTheMarkerFile();
  TheMarkerTextRoundTripsAndStaysInTheMenu();
  TheMarkerVersionIsReadFromTheMarkerFile();
  ANumberOutsideTheMenuIsClamped();
  NothingUsableIsMinusOne();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "texture_menu_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("texture_menu_test: all checks pass\n");
  return 0;
}
