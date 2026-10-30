#include "gamespace.h"

#include "safe_read.h"

namespace q15t {
namespace {
constexpr std::uint64_t kMaxGamespaces = 64, kMaxSystems = 1024;
constexpr std::uintptr_t kTables[2][2] = {{0x1f48, 0x1f78}, {0x1f88, 0x1fb8}};  // {table pointer, count} offsets in CR15Game
}  // namespace

int FindComponentSystems(std::uintptr_t game, std::uint64_t key, std::uintptr_t vptr_abs, CompSys* out, int max) {
  int n = 0;
  for (int t = 0; t < 2; ++t) {
    bool ok = true;
    const auto tab = Rd<std::uintptr_t>(game + kTables[t][0], &ok);
    const auto cnt = Rd<std::uint64_t>(game + kTables[t][1], &ok);
    if (!ok || tab == 0 || cnt > kMaxGamespaces) continue;
    for (std::uint64_t i = 0; i < cnt && n < max; ++i) {
      bool ok2 = true;
      const auto name = Rd<std::uint64_t>(tab + i * 16, &ok2);
      const auto gs = Rd<std::uintptr_t>(tab + i * 16 + 8, &ok2);
      if (!ok2 || gs == 0) continue;
      const auto sys = Rd<std::uintptr_t>(gs + 0x420, &ok2);
      const auto ns = Rd<std::uint64_t>(gs + 0x450, &ok2);
      if (!ok2 || Rd<std::uintptr_t>(gs + 0x8, &ok2) != game || !ok2 || ns > kMaxSystems) continue;
      for (std::uint64_t j = 0; j < ns && n < max; ++j) {
        bool ok3 = true;
        if (Rd<std::uint64_t>(sys + j * 16, &ok3) != key || !ok3) continue;
        const auto cs = Rd<std::uintptr_t>(sys + j * 16 + 8, &ok3);
        if (!ok3 || Rd<std::uintptr_t>(cs, &ok3) != vptr_abs || Rd<std::uintptr_t>(cs + 0x80, &ok3) != gs || !ok3) continue;
        out[n++] = {gs, cs, t, name};
      }
    }
  }
  return n;
}

}  // namespace q15t
