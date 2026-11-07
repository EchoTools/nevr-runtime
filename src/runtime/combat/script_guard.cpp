#include "runtime/combat/script_guard.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <utility>

#include "core/logging.h"
#include "runtime/combat/equip_cell.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::script_guard {
namespace {

constexpr std::int64_t kHealNodes[] = {kHealBubbleActivate};

constexpr Guard kGuards[] = {
    {"heal (Repair Matrix)", kHealScript, 0xA200, kHealDispatchPrologue, sizeof kHealDispatchPrologue, kHealSymbol,
     kHealNodes, sizeof kHealNodes / sizeof kHealNodes[0]},
};
constexpr std::size_t kCount = sizeof kGuards / sizeof kGuards[0];

using DispatchFn = std::uint64_t (*)(std::int64_t* thread);

DispatchFn g_original[kCount] = {};
std::atomic<std::uint64_t> g_skips[kCount] = {};
volatile std::uint64_t* g_cell = nullptr;
bool g_server = false;
std::atomic_bool g_registered{false};

template <std::size_t I>
std::uint64_t Dispatch(std::int64_t* thread) {
  const Guard& g = kGuards[I];
  const std::uint64_t selected = g_cell != nullptr ? g_cell[0] : 0;
  const std::int64_t node = thread[2];
  if (Skips(g, g_server, selected, node)) {
    const std::uint64_t n = g_skips[I].fetch_add(1, std::memory_order_relaxed) + 1;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<volatile std::uint8_t*>(g_cell) + kEquipSkipsOffset));
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.COMBAT] script node skipped guard=\"%s\" script=%016llx node=0x%llx selected=%016llx skips=%llu (the player "
        "selected another tac mod, so this script does not answer the use)",
        g.name, static_cast<unsigned long long>(g.script), static_cast<unsigned long long>(node),
        static_cast<unsigned long long>(selected), static_cast<unsigned long long>(n));
    return 0;  // the dispatcher's "node done": the node's work is not run
  }
  return g_original[I](thread);
}

template <std::size_t... I>
std::array<void*, sizeof...(I)> Detours(std::index_sequence<I...>) {
  return {reinterpret_cast<void*>(&Dispatch<I>)...};
}

}  // namespace

const Guard* Guards(std::size_t* count) {
  *count = kCount;
  return kGuards;
}

void Register(volatile std::uint64_t* equip_cell, bool is_server) {
  if (g_registered.exchange(true)) return;
  g_cell = equip_cell;
  g_server = is_server;
  static const auto detours = Detours(std::make_index_sequence<kCount>{});
  nevr_script_hooks::Hook rows[kCount];
  for (std::size_t i = 0; i < kCount; ++i) {
    const Guard& g = kGuards[i];
    rows[i] = nevr_script_hooks::Hook{g.script, g.dispatch_rva, g.expected, g.expected_len, detours[i],
                                      reinterpret_cast<void**>(&g_original[i]), g.name};
  }
  nevr_script_hooks::Register(rows, kCount);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] script guards registered guards=%zu server=%s (a guarded script runs its use only for the tac mod the "
      "player selected; installed as each script loads)",
      kCount, is_server ? "true" : "false");
}

std::uint64_t SkipsCounted(std::size_t guard) { return guard < kCount ? g_skips[guard].load() : 0; }

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_cell = nullptr;
  g_server = false;
  for (auto& s : g_skips) s = 0;
  for (auto& o : g_original) o = nullptr;
}
#endif

}  // namespace nevr_combat::script_guard
