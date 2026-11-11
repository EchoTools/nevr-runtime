#pragma once

// #1857, docs/standards/logging.md ([NEVR.PLUGINMGR]): what a main-menu card press looks like as a component event, as pure
// logic. The plugin manager's press hook has to know which event symbol, which owner and which component a press on a
// card carries before it can filter on them, so the observer in patch/invite_choice_hook.cpp logs these three words, at
// Debug, for every call of CncaGameSpace::SendComponentEvent. No game and no clock in here: the hook reads the event out of
// the game's memory through a reader that can refuse, and calls ReadPress and PressLine.

#include <cstdint>
#include <cstdio>
#include <string>

#include "runtime/patch/invite_choices.h"

namespace nevr_menu_press {

using Sender = nevr_invite_choices::Sender;

/// A short name for how far the sender was followed, for the log line.
inline const char* SenderName(Sender s) {
  switch (s) {
    case Sender::Unreadable: return "unreadable";
    case Sender::NoSender: return "no_sender";
    case Sender::NoTable: return "no_table";
    case Sender::OutOfRange: return "out_of_range";
    case Sender::Resolved: return "resolved";
  }
  return "unknown";
}

/// Reads the symbol, the component and the sender's owner name of the event at `event` into `out`, whatever the symbol is
/// (the wizard's observer, nevr_invite_choices::ReadEvent, reads only its own six). The symbol and the component are kept
/// when the sender does not resolve; the owner is 0 then.
template <typename Reader>
inline Sender ReadPress(Reader&& read, uintptr_t space, uintptr_t event, nevr_invite_choices::Event* out) {
  *out = nevr_invite_choices::Event{};
  uint64_t symbol = 0;
  if (event == 0 || !read(event + nevr_invite_choices::kEventSymbolOffset, &symbol, sizeof(symbol))) {
    return Sender::Unreadable;
  }
  out->symbol = symbol;
  return nevr_invite_choices::ReadSender(read, space, event, out);
}

/// The Debug line for one call: the same shape as the wizard observer's lines ("[NEVR.INVITE] wizard symbol not on a
/// wizard page read=... owner=0x... component=0x... unresolved=..."). `seen` is the hook's count of every call, the
/// identifier that orders the lines of one run.
inline std::string PressLine(const nevr_invite_choices::Event& e, Sender sender, uint64_t seen) {
  char line[256];
  std::snprintf(line, sizeof(line),
                "[NEVR.PLUGINMGR] component event read=%s event=0x%llx owner=0x%llx component=0x%llx seen=%llu",
                SenderName(sender), static_cast<unsigned long long>(e.symbol), static_cast<unsigned long long>(e.owner),
                static_cast<unsigned long long>(e.component), static_cast<unsigned long long>(seen));
  return line;
}

}  // namespace nevr_menu_press
