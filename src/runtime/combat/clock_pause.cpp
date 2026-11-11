#include "runtime/combat/clock_pause.h"

#include <atomic>
#include <cstring>
#include <utility>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::clock_pause {
namespace {

using ExpressionFn = float (*)(void* node);
using NodeFn = std::uint64_t (*)(void* node);

void* g_original[kClockCount] = {};  // each function's trampoline while hooked
std::atomic<volatile std::uint8_t*> g_mailbox{nullptr};
std::atomic_bool g_registered{false};
std::atomic<int> g_last_held{-1};  // what the last edge line said: -1 none yet, 0, 1

// Whether the clocks stand still now. One Info line when that changes, never per call: the clocks are asked every
// frame. The first answer is logged only when it is "held" (from=0), so a match that never pauses logs nothing.
bool HeldNow() {
  const rules::Pause paused = rules::RulesState(g_mailbox.load(std::memory_order_relaxed)).Paused();
  const bool held = Held(paused);
  const int now = held ? 1 : 0;
  if (g_last_held.load(std::memory_order_relaxed) != now) {
    const int was = g_last_held.exchange(now);
    if (was != now && (was >= 0 || now != 0)) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.COMBAT] clock pause: clocks held from=%d to=%d paused=%u script=%016llx (%s)", was < 0 ? 0 : was, now,
          static_cast<unsigned>(paused), static_cast<unsigned long long>(kScript),
          held ? "PAUSE is on or resuming: game_timer, lobbylock_timer and countdown_timer stand still"
               : "PAUSE is off: the clocks count down again");
    }
  }
  return held;
}

// node -> [node] the context (qword) -> [context+0x68] the globals (qword) -> + the clock's offset (a float).
std::uint8_t* TimerAt(void* node, std::uint32_t timer) {
  std::uint8_t* context = nullptr;
  std::memcpy(&context, node, sizeof context);
  std::uint8_t* globals = nullptr;
  std::memcpy(&globals, context + kContextGlobals, sizeof globals);
  return globals + timer;
}

float ReadTimer(void* node, std::uint32_t timer) {
  float value = 0;
  std::memcpy(&value, TimerAt(node, timer), sizeof value);  // movss: 4 bytes
  return value;
}

template <std::size_t I>
float Expression(void* node) {
  if (!HeldNow()) return reinterpret_cast<ExpressionFn>(g_original[I])(node);
  return HeldValue(ReadTimer(node, kClocks[I].timer));
}

template <std::size_t I>
std::uint64_t Node(void* node) {
  if (!HeldNow()) return reinterpret_cast<NodeFn>(g_original[I])(node);
  const float held = HeldValue(ReadTimer(node, kClocks[I].timer));
  if (static_cast<const std::uint8_t*>(node)[kNodeNoWrite] != 0) return ~std::uint64_t{0};
  std::memcpy(TimerAt(node, kClocks[I].timer), &held, sizeof held);  // movss: 4 bytes
  return 0;
}

template <std::size_t I>
float GameTimer(void* node) {
  if (!HeldNow()) return reinterpret_cast<ExpressionFn>(g_original[I])(node);
  const float timer = ReadTimer(node, kClocks[I].timer);
  return HeldGameTimer(timer, reinterpret_cast<ExpressionFn>(g_original[I])(node));
}

template <std::size_t I>
void* Detour() {
  switch (kClocks[I].kind) {
    case Kind::kExpression:
      return reinterpret_cast<void*>(&Expression<I>);
    case Kind::kNode:
      return reinterpret_cast<void*>(&Node<I>);
    case Kind::kGameTimer:
      return reinterpret_cast<void*>(&GameTimer<I>);
  }
  return nullptr;
}

template <std::size_t... I>
void RegisterRows(std::index_sequence<I...>) {
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kClocks[I].rva, kClocks[I].expected, kClocks[I].expected_len, Detour<I>(), &g_original[I],
       kClocks[I].name}...,
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
}

}  // namespace

bool Held(rules::Pause paused) { return paused != rules::Pause::kOff; }

float HeldValue(float timer) { return timer >= 0.0f ? timer : 0.0f; }

float HeldGameTimer(float timer, float result) {
  if (std::memcmp(&timer, &result, sizeof timer) == 0) return result;
  return HeldValue(timer);
}

void Register(volatile std::uint8_t* mailbox) {
  if (g_registered.exchange(true)) return;
  g_mailbox.store(mailbox);
  RegisterRows(std::make_index_sequence<kClockCount>{});
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] clock pause fix registered script=%016llx hooks=%zu (while PAUSE is on or resuming, game_timer, "
      "lobbylock_timer and countdown_timer stand still; installed when the game-mode script loads)",
      static_cast<unsigned long long>(kScript), kClockCount);
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_mailbox.store(nullptr);
  for (void*& original : g_original) original = nullptr;
  g_last_held = -1;
}
#endif

}  // namespace nevr_combat::clock_pause
