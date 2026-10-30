// Host test of the game-tick table (src/quest/plugins/q15t/src/game_tick.cpp): 16 callbacks fit, the 17th is refused, and the wrapper runs all 16 in
// registration order after the original, only for an object that carries the CR15Game vptr. Real code, real mprotect, a fake slot page.
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "game_tick.h"
#include "plugin_log.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                 \
  do {                                                   \
    if (!(cond)) {                                       \
      ++g_fail;                                          \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                          \
      std::printf("\n");                                 \
    }                                                    \
  } while (0)

static std::vector<std::string> g_lines;
static void FakeLog(const char*, uint32_t, const char* msg, const NevrLogField*, uint32_t) { g_lines.push_back(msg); }

static std::vector<int> g_order;  // 0 = the original, 1..16 = the callbacks
static int g_real_calls = 0;
extern "C" __attribute__((noinline, visibility("default"))) void FakeExclusiveUpdateGame(void*) {
  ++g_real_calls;
  g_order.push_back(0);
}

template <int N>
static void Tick(std::uintptr_t) {
  g_order.push_back(N);
}

using q15t::TickFn;
static const TickFn kTicks[16] = {&Tick<1>, &Tick<2>,  &Tick<3>,  &Tick<4>,  &Tick<5>,  &Tick<6>,  &Tick<7>,  &Tick<8>,
                                  &Tick<9>, &Tick<10>, &Tick<11>, &Tick<12>, &Tick<13>, &Tick<14>, &Tick<15>, &Tick<16>};

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  CHECK(q15t::kMaxGameTicks == 16, "kMaxGameTicks is %d, want 16", q15t::kMaxGameTicks);
  CHECK(q15t::GameTickCount() == 0, "table not empty at start: %d", q15t::GameTickCount());
  for (int i = 0; i < 16; ++i) {
    CHECK(q15t::AddGameTick(kTicks[i]), "callback %d refused (the table must take 16)", i + 1);
    CHECK(q15t::GameTickCount() == i + 1, "count after %d adds is %d", i + 1, q15t::GameTickCount());
  }
  CHECK(!q15t::AddGameTick(&Tick<17>), "the 17th callback was accepted");
  CHECK(!q15t::AddGameTick(&Tick<18>), "the 18th callback was accepted");
  CHECK(q15t::GameTickCount() == 16, "count after refusals is %d, want 16", q15t::GameTickCount());

  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* page = static_cast<uintptr_t*>(p);
  const uintptr_t base = reinterpret_cast<uintptr_t>(page);
  page[4] = reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame);  // the slot, at byte offset 0x20
  mprotect(p, 4096, PROT_READ);
  const auto r = q15t::InstallGameTick(base, 0x20, reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame) - base, 0x100);
  CHECK(r.status == q15t::SwapStatus::kOk, "InstallGameTick status %s", q15t::StatusName(r.status));
  auto wrapper = reinterpret_cast<void (*)(void*)>(page[4]);
  CHECK(wrapper != &FakeExclusiveUpdateGame, "slot not replaced");

  static uintptr_t game[8] = {};
  game[0] = base + 0x100;  // the CR15Game vptr value
  g_order.clear();
  wrapper(game);
  bool in_order = g_order.size() == 17;
  for (int i = 0; in_order && i < 17; ++i) in_order = g_order[i] == i;
  CHECK(in_order && g_real_calls == 1, "the original then callbacks 1..16 once each, in order: got %zu entries, %d original calls",
        g_order.size(), g_real_calls);

  static uintptr_t other[8] = {};
  other[0] = 0x777;
  g_order.clear();
  g_lines.clear();
  wrapper(other);
  CHECK(g_order.size() == 1 && g_order[0] == 0, "an object without the CR15Game vptr ran a callback (%zu entries)", g_order.size());
  bool loud = false;
  for (const auto& l : g_lines) loud = loud || l.find("not the CR15Game") != std::string::npos;
  CHECK(loud, "no ERROR line for the wrong vptr");
  CHECK(q15t::GameTickCalls() == 2, "wrapper calls %u, want 2", q15t::GameTickCalls());

  if (g_fail == 0) std::printf("game_tick_test: all checks pass (16 callbacks, the 17th refused)\n");
  else std::printf("game_tick_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
