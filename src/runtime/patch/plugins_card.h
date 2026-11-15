#pragma once

// #1857, docs/standards/logging.md ([NEVR.PLUGINMGR]): the press hook's decision, as pure logic. The PLUGINS card on the main
// menu is a clone of the TUTORIALS card (the fifth-card override set). A press on a card is one call of
// CncaGameSpace::SendComponentEvent with the symbol evt_card_clicked, the page actor as owner and the card's own component
// id; every card sends the same symbol and the same owner, so the component is the one word that tells them apart (measured
// on a PC client: target and value are the same for every card). The observer in patch/invite_choice_hook.cpp calls
// ReadPluginsPress for every call: it reads the symbol, the component of a card click, and walks the sender only for a press
// on this card. No game and no clock in here.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "runtime/patch/invite_choices.h"
#include "runtime/patch/menu_press.h"

namespace nevr_plugins_card {

// The component id the override set gives the PLUGINS card: the first 8 bytes of the card's entry in the mnu_master
// CR15UIPage2ElementCR override. That entry is the source of truth; tools/tests/test_plugins_card_override.py fails when
// this number and the entry differ. It must not equal kTutorialsCardComponent: the clone first shared it, and a press on
// PLUGINS then started the tutorial.
inline constexpr uint64_t kPluginsCardComponent = 0x35ec861316abbbf1ULL;
// The TUTORIALS card's component id in the game's own resource, the card the PLUGINS card was cloned from.
inline constexpr uint64_t kTutorialsCardComponent = 0x35ec861316abbb35ULL;

/// True for a click on the PLUGINS card: a card click whose component is the card's. The sender is not part of it, so the
/// decision holds whether or not the sender resolved. A press the head left with component 0 (not a card click) is never one.
inline bool IsPluginsPress(const nevr_menu_press::Press& press) {
  return press.symbol == nevr_invite_choices::kEvtCardClicked && press.component == kPluginsCardComponent;
}

/// The hook's per-call work: reads the head of the event, decides, and only for a press on the PLUGINS card follows the
/// sender (`*sender` and `press->owner`). Returns true for that press. An event that is not a card click costs one read, a
/// card click on another card two, both inside the event; neither reaches the sender walk.
template <typename Reader>
inline bool ReadPluginsPress(Reader&& read, uintptr_t space, uintptr_t event, nevr_menu_press::Press* press,
                             nevr_menu_press::Sender* sender) {
  *sender = nevr_menu_press::Sender::Unreadable;
  if (!nevr_menu_press::ReadHead(read, event, press) || !IsPluginsPress(*press)) return false;
  *sender = nevr_menu_press::ResolveOwner(read, space, event, press);
  return true;
}

// The longest line: the text below with the two 16-digit ids, a 20-digit count and the longest sender name, with room to spare.
inline constexpr size_t kPluginsLineBytes = 256;

/// The one Info line for a press, written into `line` (at most `size` bytes, NUL included): the tag, the component, the
/// page actor that sent it and how far the sender was followed (`owner=0x0` when it did not resolve), and `seen`, the
/// hook's count of every call, which orders the lines of one run. The consequence is in the line: nothing opens yet. Returns
/// the length the line has; a value of `size` or more means it was cut. No heap.
inline size_t PluginsPressLine(char* line, size_t size, const nevr_menu_press::Press& press,
                               nevr_menu_press::Sender sender, uint64_t seen) {
  const int written = std::snprintf(
      line, size,
      "[NEVR.PLUGINMGR] plugins card pressed component=0x%llx owner=0x%llx read=%s seen=%llu (no page opens yet; the "
      "press is only recorded)",
      static_cast<unsigned long long>(press.component), static_cast<unsigned long long>(press.owner),
      nevr_menu_press::SenderName(sender), static_cast<unsigned long long>(seen));
  return written < 0 ? size : static_cast<size_t>(written);
}

}  // namespace nevr_plugins_card
