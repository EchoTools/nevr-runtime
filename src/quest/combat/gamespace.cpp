#include "quest/combat/gamespace.h"

#include "quest/combat/combat_level_targets.h"
#include "quest/combat/gamespace_targets.h"
#include "quest/combat/safe_read.h"

namespace nevr_quest::combat::gamespace {

namespace {

namespace pin = level::targets;
constexpr std::uintptr_t kTables[2][2] = {{pin::kTable0Rows, pin::kTable0Count}, {pin::kTable1Rows, pin::kTable1Count}};
constexpr std::uint64_t kMaxGamespaces = 64, kMaxSystems = 1024;
constexpr std::uintptr_t kRowGamespace = 8;
constexpr std::uintptr_t kSystems = targets::kSystems, kSystemCount = targets::kSystemCount, kSystemGamespace = targets::kSystemGamespace;

}  // namespace

int FindComponentSystems(std::uintptr_t game, std::uint64_t key, std::uintptr_t vptrAbs, CompSys* out, int max) noexcept {
  int n = 0;
  for (int t = 0; t < 2; ++t) {
    bool ok = true;
    const auto tab = SafeRead<std::uintptr_t>(game + kTables[t][0], &ok);
    const auto cnt = SafeRead<std::uint64_t>(game + kTables[t][1], &ok);
    if (!ok || tab == 0 || cnt > kMaxGamespaces) continue;
    for (std::uint64_t i = 0; i < cnt && n < max; ++i) {
      bool ok2 = true;
      const auto name = SafeRead<std::uint64_t>(tab + i * 16, &ok2);
      const auto gs = SafeRead<std::uintptr_t>(tab + i * 16 + kRowGamespace, &ok2);
      if (!ok2 || gs == 0) continue;
      const auto sys = SafeRead<std::uintptr_t>(gs + kSystems, &ok2);
      const auto ns = SafeRead<std::uint64_t>(gs + kSystemCount, &ok2);
      if (!ok2 || SafeRead<std::uintptr_t>(gs + pin::kGamespaceOwnerOffset, &ok2) != game || !ok2 || ns > kMaxSystems) continue;
      for (std::uint64_t j = 0; j < ns && n < max; ++j) {
        bool ok3 = true;
        if (SafeRead<std::uint64_t>(sys + j * 16, &ok3) != key || !ok3) continue;
        const auto cs = SafeRead<std::uintptr_t>(sys + j * 16 + 8, &ok3);
        if (!ok3 || SafeRead<std::uintptr_t>(cs, &ok3) != vptrAbs || SafeRead<std::uintptr_t>(cs + kSystemGamespace, &ok3) != gs || !ok3) continue;
        out[n++] = {gs, cs, t, name};
      }
    }
  }
  return n;
}

}  // namespace nevr_quest::combat::gamespace
