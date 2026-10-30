// The Quest's side of the UI text writer (#1726, docs/adr/0017 "Show the result"; the PC's is runtime/patch/ui_text.cpp).
//
// The writer is one: the queue, the actor lookup, the write and the log lines are runtime/patch/ui_text.h, the same code the PC
// runs. What differs by binary is the address table and the memory it reads, and that is all this file holds: the three game
// calls at their libr15 addresses (sentinel/pinned_targets.h, each behind the words it is pinned by), the game object (the
// frame hook's netgame +8) and the Quest's actor table (nevr_ui_text::FindActorInSortedMap). There is no second writer.
//
// Game thread only (the frame dispatcher, frame_dispatch.h). The writer finds its calls on the first post, writes one
// `[NEVR.UI] text writer ready|off ...` line either way, and costs one atomic load per frame until something is posted.
#pragma once

#include <cstdint>
#include <string>

#include "runtime/patch/ui_text.h"

namespace nevr_quest::ui_text {

/// The game, behind one seam: production reads libr15 and the frame hook's netgame; the host test fakes it.
class GameCalls {
 public:
  virtual ~GameCalls() = default;
  /// Checks the three calls' pinned words in the running libr15. Fills `state` with one of ok, not_found, prologue_mismatch for
  /// each, in the order game_space, find_canvas, set_text. True when all three are ok.
  virtual bool Pin(const char* (&state)[3]) = 0;
  virtual std::uintptr_t Game() = 0;  // the game object (a CR15Game), 0 before the first frame
  virtual std::uintptr_t GameSpace(std::uintptr_t game, std::uint64_t index) = 0;
  virtual bool Read(std::uintptr_t address, void* out, std::size_t length) = 0;
  virtual bool FindCanvas(std::uintptr_t gs, std::uint64_t ident, std::uintptr_t* canvas, std::uint32_t* handle) = 0;
  virtual bool SetText(std::uintptr_t canvas, std::uint32_t handle, std::uint64_t element, const char* text) = 0;
  virtual std::uint64_t NowMs() = 0;
  virtual std::string FirstWriteLine() = 0;
  virtual void Log(nevr_ui_text::LogLevel level, const std::string& line) = 0;
};

/// The production seam.
GameCalls& LiveCalls();

/// True once the three calls are pinned (the first call checks them and logs the outcome). False while the writer is off.
bool Ready();

/// Queues `request` for the next frame (nevr_ui_text::Pending::Post). Call only when Ready().
nevr_ui_text::Check Post(nevr_ui_text::Request request);

/// Tries the requests that are due and logs each one that is done. Game thread. One atomic load while nothing was ever posted.
void OnFrame();

/// Test support: use `calls` from the next call on (null restores the live seam), and forget the pinned state and the queue.
void ResetForTest(GameCalls* calls);

}  // namespace nevr_quest::ui_text
