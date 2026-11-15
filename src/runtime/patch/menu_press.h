#pragma once

// #1857, docs/standards/logging.md ([NEVR.PLUGINMGR]): what a main-menu card press looks like as a component event, as pure
// logic. CncaGameSpace::SendComponentEvent has about 25 callers (grab events included; event-driven callers, rate not
// measured), so what the plugin manager's press hook does for every call has to be nearly free: ReadHead reads the event symbol and, for a
// card click only, the component, and both words are inside the event, which the game itself has just read. The sender walk
// (ResolveOwner: four reads outside the event, a page query each in the hook) is a separate step the caller takes only after
// the head says the event is worth it. No game and no clock in here: the hook passes a reader that can refuse.

#include <cstdint>

#include "runtime/patch/invite_choices.h"

namespace nevr_menu_press {

using Sender = nevr_invite_choices::Sender;

/// The words of one call this feature uses: the symbol and the component (read by ReadHead), the sender's owner name
/// (resolved by ResolveOwner, 0 until it is).
using Press = nevr_invite_choices::Event;

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

/// Reads the symbol of the event at `event` into `out`, and the component too when the symbol is a card click (every other
/// event leaves the component 0, so no decision on it can match). Reads nothing outside the event and never the sender.
/// False when `event` is null or a word cannot be read; `out` is then whatever was read before.
template <typename Reader>
inline bool ReadHead(Reader&& read, uintptr_t event, Press* out) {
  *out = Press{};
  uint64_t symbol = 0;
  if (event == 0 || !read(event + nevr_invite_choices::kEventSymbolOffset, &symbol, sizeof(symbol))) return false;
  out->symbol = symbol;
  if (symbol != nevr_invite_choices::kEvtCardClicked) return true;
  uint64_t component = 0;
  if (!read(event + nevr_invite_choices::kEventComponentOffset, &component, sizeof(component))) return false;
  out->component = component;
  return true;
}

/// Follows the sender of `event` to the name of its actor and stores it in `out->owner` (the component is stored too). The
/// walk the head avoids: take it only for an event that has already been matched.
template <typename Reader>
inline Sender ResolveOwner(Reader&& read, uintptr_t space, uintptr_t event, Press* out) {
  return nevr_invite_choices::ReadSender(read, space, event, out);
}

}  // namespace nevr_menu_press
