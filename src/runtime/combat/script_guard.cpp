#include "runtime/combat/script_guard.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <utility>

#include "core/logging.h"
#include "runtime/combat/equip_cell.h"
#include "runtime/hook/script_hook_calls.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::script_guard {
namespace {

constexpr std::int64_t kHealNodes[] = {kHealBubbleActivate};
constexpr std::int64_t kBarrierNodes[] = {kBarrierCustomParam, kBarrierWallCreate};

// The table. Index 0 is the stock heal (Repair Matrix), which the install carries; the rest are copies (see the header).
constexpr Guard kGuards[] = {
    {"heal (Repair Matrix)", kHealScript, kHealDispatchRva, kHealDispatchPrologue, sizeof kHealDispatchPrologue,
     kHealSymbol, kHealNodes, sizeof kHealNodes / sizeof kHealNodes[0]},
    {"barrier copy (UMBRAL VEIL)", kBarrierSmokeScript, kBarrierDispatchRva, kBarrierDispatchPrologue,
     sizeof kBarrierDispatchPrologue, kSmokeSymbol, kBarrierNodes, sizeof kBarrierNodes / sizeof kBarrierNodes[0]},
    {"barrier copy (NEUTRON)", kBarrierPointScript, kBarrierDispatchRva, kBarrierDispatchPrologue,
     sizeof kBarrierDispatchPrologue, kPointSymbol, kBarrierNodes, sizeof kBarrierNodes / sizeof kBarrierNodes[0]},
    {"heal copy (OVERCHARGE)", kOverchargeScript, kHealDispatchRva, kHealDispatchPrologue,
     sizeof kHealDispatchPrologue, kBuffSymbol, kHealNodes, sizeof kHealNodes / sizeof kHealNodes[0]},
};
constexpr std::size_t kCount = sizeof kGuards / sizeof kGuards[0];

// The dispatcher's argument is the game's thread record; the node id is its third word (the stock dispatcher reads it there).
constexpr std::size_t kNodeIdSlot = 2;

using DispatchFn = std::uint64_t (*)(std::int64_t* thread);

DispatchFn g_original[kCount] = {};
std::atomic<std::uint64_t> g_skips[kCount] = {};
volatile std::uint64_t* g_cell = nullptr;
bool g_server = false;
std::atomic_bool g_registered{false};

template <std::size_t I>
std::uint64_t Dispatch(std::int64_t* thread) {
  NEVR_SCRIPT_HOOK_CALL(kGuards[I].name);
  const Guard& g = kGuards[I];
  const std::uint64_t selected = g_cell != nullptr ? g_cell[0] : 0;
  const std::int64_t node = thread[kNodeIdSlot];
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

std::size_t Plan(const std::uint64_t* held, std::size_t held_count, std::size_t* out) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < kCount; ++i) {
    for (std::size_t h = 0; h < held_count; ++h) {
      if (held[h] == kGuards[i].script) {
        out[n++] = i;
        break;
      }
    }
  }
  return n;
}

void RegisterPlanned(volatile std::uint64_t* equip_cell, bool is_server, const std::uint64_t* held, std::size_t held_count) {
  if (g_registered.exchange(true)) return;
  g_cell = equip_cell;
  g_server = is_server;
  static const auto detours = Detours(std::make_index_sequence<kCount>{});
  std::size_t picked[kCount] = {};
  const std::size_t n = Plan(held, held_count, picked);
  nevr_script_hooks::Hook rows[kCount];
  for (std::size_t k = 0; k < n; ++k) {
    const std::size_t i = picked[k];
    const Guard& g = kGuards[i];
    rows[k] = nevr_script_hooks::Hook{g.script, g.dispatch_rva, g.expected, g.expected_len, detours[i],
                                      reinterpret_cast<void**>(&g_original[i]), g.name};
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.COMBAT] script guard row script=%016llx dispatch=0x%x own=%016llx nodes=%zu guard=\"%s\" (its script is "
        "held; the row installs when the script loads)",
        static_cast<unsigned long long>(g.script), g.dispatch_rva, static_cast<unsigned long long>(g.own), g.node_count,
        g.name);
  }
  nevr_script_hooks::Register(rows, n);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] script guards registered guards=%zu server=%s (a guarded script runs its use only for the tac mod the "
      "player selected; installed as each script loads)",
      n, is_server ? "true" : "false");
}

void Register(volatile std::uint64_t* equip_cell, bool is_server) {
  std::uint64_t all[kCount] = {};
  for (std::size_t i = 0; i < kCount; ++i) all[i] = kGuards[i].script;
  RegisterPlanned(equip_cell, is_server, all, kCount);
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
