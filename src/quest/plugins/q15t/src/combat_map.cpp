#include "combat_map.h"

#include "safe_read.h"

namespace q15t {
namespace {

// mpl_combat_dyson / _combustion / _fission / _gauss: the level CSymbol64 a gamespace holds at gs+0x58.
constexpr std::uint64_t kCombatLevels[4] = {0x43e2da7914642604ull, 0x42670f2bed45703cull, 0xdf5ca7b7dfa383d4ull,
                                            0x43e2da7a0c623a19ull};
// CR15Game's two gamespace tables {sym, gs*} (16-byte rows) and their counts (the pinned build).
constexpr std::uintptr_t kTables[2][2] = {{0x1f48, 0x1f78}, {0x1f88, 0x1fb8}};
constexpr std::uint64_t kMaxGamespaces = 64;

}  // namespace

bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level) {
  for (const auto& t : kTables) {
    bool ok = true;
    const auto tab = Rd<std::uintptr_t>(game + t[0], &ok);
    const auto cnt = Rd<std::uint64_t>(game + t[1], &ok);
    if (!ok || tab == 0 || cnt > kMaxGamespaces) continue;
    for (std::uint64_t i = 0; i < cnt; ++i) {
      bool ok2 = true;
      const auto gs = Rd<std::uintptr_t>(tab + i * 16 + 8, &ok2);
      if (!ok2 || gs == 0) continue;
      if (Rd<std::uintptr_t>(gs + 0x8, &ok2) != game || !ok2) continue;
      const auto lv = Rd<std::uint64_t>(gs + 0x58, &ok2);
      if (!ok2) continue;
      for (const std::uint64_t c : kCombatLevels) {
        if (lv == c) {
          *level = lv;
          return true;
        }
      }
    }
  }
  *level = 0;
  return false;
}

}  // namespace q15t
