/* The Texture Resolution control in the game's own settings menu (#1012), the pure half: the two JSON documents the game
 * reads its settings menu from, rewritten in memory, and the player's saved choice read back.
 *
 * The design is marshmallow-mia's (EchoTools/echoApkPatch, tools/patch_menu_json.py); only facts are taken from it.
 *
 * What the game does (pinned libr15; texture_menu_hook.h has the addresses). The settings page list comes from
 * json/r14/config/uisettings/menu_settings.json (CR15UISettingsCS::InitCI), and which settings and pages a platform hides from
 * json/r14/config/gamesettings_config.json (CR15SettingsTranslatorCS::Initialize). Both are read through
 * CJson::DecodeFrom(CFileSp const&, uint), which reads the file, strips its comments and parses the text with
 * CJson::DecodeFrom(char const*, unsigned long long). The graphics setting `graphics|msaa` is the field SR15GfxSettings+0x1c:
 * ApplyRuntimeGfxSettings sets the render's MSAA mode from two constants (both 2 in the store build) and reads that field only
 * for a log line, so the field can carry the texture level without touching anything else. On a Quest the stock files hide it
 * (the quest `settings` list removes `graphics|msaa`, the quest `pages` list removes the page "GRAPHICS (1/3)" that holds it).
 *
 *   menu_settings.json    the `graphics|msaa` row becomes the six-level "Texture Resolution" STRING ARRAY (labels and the default
 *                         index in texture_quality.h), and the page that holds that row keeps that row and one more, and is
 *                         called "GRAPHICS" (the other graphics rows are not functional on a Quest and stay hidden). The one more
 *                         is "Controller Hand Flip", a STRING ARRAY On / Off bound to `graphics|fullscreen` (hand_flip.h).
 *   gamesettings_config.json  the quest `settings` entry that removes `graphics|msaa` and the quest `pages` entry that removes the
 *                         page "GRAPHICS (1/3)" go, so the control is shown and its value saved; so does any `settings` entry
 *                         that removes `graphics|fullscreen`, for the hand flip row.
 *
 * The documents are the headset's own: they are read in memory from what the game delivered, edited by structure (never by a
 * pinned hash of a file, never from stored copies of Ready At Dawn's data), and handed on. A document that does not have the
 * expected shape is left alone and the result says so.
 *
 * Pure: nlohmann::json and the standard library, built with exceptions like quest_config.cpp. The hook's translation unit
 * (texture_menu_hook.cpp, -fno-exceptions) reaches it only through a function marked NEVR_OUTSIDE_GAME_CALL, before it calls the
 * game; every call here is one that does not throw (parse with allow_exceptions = false, no at(), dump with the replace handler).
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace nevr_quest::texture_menu {

// The setting the control is bound to, and the stock page that holds its row (the page the quest `pages` list removes).
inline constexpr const char* kSettingPath = "graphics|msaa";
inline constexpr const char* kStockGraphicsPage = "GRAPHICS (1/3)";

enum class Status {
  kRewritten,      // the document was changed; `text` holds the new document
  kNotJson,        // the text did not parse; nothing changed
  kShapeMismatch,  // it parsed but has no `graphics|msaa` row (menu) or removal (settings); nothing changed
};

const char* StatusName(Status status) noexcept;

struct Result {
  Status status = Status::kNotJson;
  std::string text;          // the rewritten document, when status == kRewritten
  std::string page;          // menu: the name of the page that held the row ("" when none was named)
  int rows = 0;              // menu: `graphics|msaa` rows turned into the control; settings: removals made
  int pages = 0;             // menu: pages reduced to the one row; settings: page removals made
  int handFlipRows = 0;      // KNOWN-WRONG #2043 menu: Controller Hand Flip rows added under Texture Resolution; settings: its removals made
};

// menu_settings.json -> the Texture Resolution control, and under it, on the same page, the Controller Hand Flip row (hand_flip.h;
// KNOWN-WRONG #2043 the row is the interim toggle). `defaultIndex` is the control's default selected index (the "idx" field the
// engine honours as the default); `handFlipIndex` is the toggle's (the row lists On first: 0 On, 1 Off, the default; the player's stored
// choice when it was resolved before the menu was read). kNoHandFlipRow adds no row: the toggle is not offered when the hand pose
// hooks that roll, save and log it are not installed (hand_flip.h, Offered).
inline constexpr int kNoHandFlipRow = -1;  // KNOWN-WRONG #2043 the toggle is the interim
Result RewriteMenuSettings(std::string_view text, int defaultIndex, int handFlipIndex = 1);

// gamesettings_config.json -> the entries that hide the control and its page removed, and, when `offerHandFlip`, any entry that hides
// the Controller Hand Flip row's setting (KNOWN-WRONG #2043: with no row offered the stock entry stays).
Result RewriteGameSettings(std::string_view text, bool offerHandFlip = true);

// The player's choice, kept in a file of our own beside the game's settings (RAD/LoneEcho/ under the game's local app data
// directory). The game's own saved slot (`msaa` in settings_mp_v2_quest.json) cannot say whether the player chose: a settings file
// that never held the setting reads 0, which is the Very Low entry. The marker exists only after the player changed the control.
inline constexpr char kMarkerFile[] = "nevr_texture_quality.json";

// The marker's format version. A marker that carries it is the player's choice made in the menu. A marker without it (builds that
// wrote `{"menu_index":N}` alone also wrote it for a level they derived themselves, so it cannot be told from a choice) or with another
// version is not read.
inline constexpr int kMarkerVersion = 1;

// The text the marker file holds for a menu index (clamped to the menu's range): {"version":1,"menu_index":N}.
std::string MarkerText(int menuIndex) noexcept;

// The value of the first `"menu_index"` key in a marker file's text, clamped to the menu's range, or -1 when there is none or it is
// not a whole number. Allocation-free: it runs on the game's first eye-size query. It does not look at the version.
int MarkedMenuIndex(const char* text, std::size_t size) noexcept;

// The value of the first `"version"` key in a marker file's text, or 0 when there is none or it is not a whole number (an
// unversioned marker reads 0). Allocation-free.
int MarkerVersion(const char* text, std::size_t size) noexcept;

}  // namespace nevr_quest::texture_menu
