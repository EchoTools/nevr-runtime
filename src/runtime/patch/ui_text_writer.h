#pragma once
// The game-facing half of the UI text writer (#896, #906, docs/adr/0017). patch/ui_text.h is the pure logic; this is its
// platform: the game's own functions, pinned by their bytes, and the entry points the rest of the runtime uses. A text is
// posted from any thread and written on the game-loop thread by the per-frame dispatcher (frame/tick.cpp).

#include <cstdint>

#include "runtime/patch/ui_text.h"

namespace nevr_ui_text {

/// Pins the three game functions the writer calls (CR15Game::GetGameSpace, the canvas component Find, CCanvasUICS::SetText)
/// and, when all three match, makes the writer ready. Client only. Logs the result either way ([NEVR.UI]).
void Install(std::uintptr_t gameBase);

/// True once Install pinned everything. Post before that is refused by the caller, which says why.
bool Ready();

/// Queues `request` (nevr_ui_text::Pending::Post with the process clock). Any thread.
Check Post(Request request);

/// Tries the requests that are due and logs each one that is done. Game-loop thread only (frame/tick.cpp gates it).
void OnFrame();

}  // namespace nevr_ui_text
