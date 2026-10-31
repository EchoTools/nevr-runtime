#pragma once
// What the menu's version text becomes, apart from the hook that applies it (menu_version_label.cpp) so it is
// unit-tested without the game.
//
// Evidence (disassembly of the game binaries; PC VAs are echovr.exe):
//   - The stock menu already has a versions page: script DLL f0c9097e9973b51b.dll (symbol ui_mnu_versions) sets three
//     text items on the CanvasUI component of actor mnu_versions (0xf4db20fee634bd0f) each time its page element is
//     enabled, and sets them to "" when it is disabled: publisher_lock, lobby_version, rad_version. The text comes
//     from R15NetVersionsExpression (PC body 0x140da6660): lobby_version is ISO8601Utc(...) of the lobby build time,
//     rad_version is the integer 631547, publisher_lock is the net-game document's "publisher_lock" ("rad15_live").
//   - Every SetText2 script node reaches CCanvasUICS::SetText(handle, CSymbol64 item, const char* text) at
//     echovr.exe 0x14037d470 (CSetText2Node execute, 0x14126dce0, calls it at 0x14126e724). Its text slot is
//     copied with a per-item capacity (CUICanvasInstance primitive text copy, 0x140727f10: at most capacity - 1
//     bytes), so a text that is too long is cut, never overrun.
//   - The item symbol lobby_version is used by that one script only (whole-project search, 1,041 binaries).
// Not established from static data: that the page holding the mnu_versions actor is shown on the main menu, and the
// capacity of the lobby_version slot. The hook logs every replacement so a run settles both.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nevr_menu_version_label {

// Symbol corpus: lobby_version.
constexpr std::uint64_t kLobbyVersionItem = 0x457626a49b59152aULL;

// Symbol corpus: publisher_lock (symcache_data.cpp). The panel draws the net-game document's publisher lock there, which
// is "rad15_live" (login_rewrite.cpp keeps it on the wire). This is the only item of the three that can hold "rad15_live":
// lobby_version holds the build timestamp this file replaces, rad_version an integer.
constexpr std::uint64_t kPublisherLockItem = 0xa6fbcbfe579dd357ULL;

// The text the panel draws for the publisher lock: the community service's name. Display only; the document keeps its value.
constexpr const char* kPublisherLockLabel = "echovrce";

// The longest text the lobby_version slot is assumed to hold. The game writes an ISO 8601 UTC timestamp there
// ("2023-04-04T17:47:47+00:00", 25 characters), so the slot holds at least 24 characters plus the terminator.
// Assumption, not a measurement: a slot that is smaller cuts the label, it does not overrun.
constexpr std::size_t kMaxLabelChars = 24;

// The label as it will be drawn: whole when it fits; otherwise without the "+<commit>" build metadata (the commit
// stays in the log line); otherwise cut to the width.
inline std::string FitLabel(std::string_view label, std::size_t maxChars = kMaxLabelChars) {
  if (label.size() <= maxChars) return std::string(label);
  const std::size_t plus = label.find('+');
  if (plus != std::string_view::npos) label = label.substr(0, plus);
  if (label.size() > maxChars) label = label.substr(0, maxChars);
  return std::string(label);
}

// The text to draw for one SetText call, or nullptr to leave the call alone. Only the lobby_version item with a
// non-empty text is replaced: the empty text is the script clearing the item when its page is disabled, and it
// must stay empty.
inline const char* ReplacementFor(std::uint64_t item, const char* text, const std::string& fittedLabel) {
  if (item != kLobbyVersionItem) return nullptr;
  if (text == nullptr || text[0] == '\0') return nullptr;
  if (fittedLabel.empty()) return nullptr;
  return fittedLabel.c_str();
}

// The text to draw for the publisher lock item, or nullptr to leave the call alone. The empty text is the script clearing
// the item, and it stays empty.
inline const char* PublisherLockReplacement(std::uint64_t item, const char* text) {
  if (item != kPublisherLockItem) return nullptr;
  if (text == nullptr || text[0] == '\0') return nullptr;
  return kPublisherLockLabel;
}

}  // namespace nevr_menu_version_label
