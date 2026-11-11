// The combat tick and the game's levels for a combat host test of a module that reads the combat level answer (src/quest/combat/combat_scope.h): the
// tick's slot row for the libr15 image the test builds, the call that registers the scope's refresh with the tick on that image (what the shipped table's
// `scope` row does before the module), a model of CR15Game's gamespace tables (combat_game_model.h) and a clock the scope reads. `Levels` moves the game to
// the named levels and runs the refresh once, so the answer the module reads is the one a real look made.
#pragma once

#include <cstdint>
#include <initializer_list>

#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/combat_tick_targets.h"
#include "quest/tests/combat_game_model.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace combat_test::scope_world {

namespace scope = nevr_quest::combat::scope;
namespace tick = nevr_quest::combat::tick;

inline GameModel& Model() {
  static GameModel model;
  return model;
}
inline std::int64_t& NowMs() {
  static std::int64_t now = 1000;
  return now;
}
inline std::int64_t Clock() noexcept { return NowMs(); }

// The row of the game-thread tick's slot, for `Image::Build`, and the call that puts the game's own frame function into it (any address in an executable
// mapping).
inline Reloc TickRow() {
  namespace tt = tick::targets;
  return {tt::kSymbol, tt::kSlotVaddr, false, false, tt::kStockVaddr};
}
inline void FakeTickFrame(void*) {}
inline void PutTickSlot(Image& image) { *image.Slot(tick::targets::kSlotVaddr) = reinterpret_cast<void*>(&FakeTickFrame); }

// Forgets the tick, the scope and the model; the clock starts at 1000 ms.
inline void Reset() {
  Model().Clear();
  NowMs() = 1000;
  tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&Clock);
}

// The tick is installed on the image `lookup` finds and the scope row has registered the refresh: what the shipped table does before a module that reads
// the scope. True when the answer can change.
inline bool StartScope(sentinel::ImageLookup lookup) {
  return tick::InstallAt(lookup) && scope::InstallNormal().installed && scope::Live();
}

// The game goes to these levels, and the combat tick's callback runs once (the clock moves past the look spacing).
inline void Levels(std::initializer_list<const char*> names) {
  Model().Only(names);
  NowMs() += scope::kLookEveryMs;
  scope::Refresh(Model().Address());
}

// The game's levels change and nothing looks: the answer stays the last look's.
inline void LevelsWithoutALook(std::initializer_list<const char*> names) { Model().Only(names); }

// The tick stops: the clock moves past the staleness limit and no refresh runs.
inline void TheTickStops() { NowMs() += scope::kStaleAfterMs + 1; }

}  // namespace combat_test::scope_world
