#pragma once

// #1857, docs/standards/logging.md ([NEVR.PLUGINMGR]): the press hook's decision, as pure logic. The PLUGINS card on the main
// menu is a clone of the TUTORIALS card (the fifth-card override set). A press on a card is one call of
// CncaGameSpace::SendComponentEvent with the symbol evt_card_clicked, the page actor as owner and the card's own component
// id; every card sends the same symbol and the same owner, so the component is the one word that tells them apart (measured
// on a PC client: target and value are the same for every card). The observer in patch/invite_choice_hook.cpp reads the
// event with nevr_menu_press::ReadPress and asks IsPluginsPress. No game and no clock in here.

#include <cstdint>
#include <cstdio>
#include <string>

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
/// decision holds whether or not the sender resolved.
inline bool IsPluginsPress(const nevr_menu_press::Press& press) {
  return press.symbol == nevr_invite_choices::kEvtCardClicked && press.component == kPluginsCardComponent;
}

inline constexpr size_t kPluginsLineBytes = 192;

/// The one Info line for a press: the tag, the component and `seen`, the hook's count of every call, which orders the
/// lines of one run. The consequence is in the line: nothing opens yet.
inline std::string PluginsPressLine(const nevr_menu_press::Press& press, uint64_t seen) {
  char line[kPluginsLineBytes];
  std::snprintf(line, sizeof(line),
                "[NEVR.PLUGINMGR] plugins card pressed component=0x%llx seen=%llu (no page opens yet; the press is only "
                "recorded)",
                static_cast<unsigned long long>(press.component), static_cast<unsigned long long>(seen));
  return line;
}

}  // namespace nevr_plugins_card
