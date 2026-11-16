#include "runtime/combat/dot_trigger.h"

#include <atomic>

#include "core/logging.h"
#include "runtime/hook/script_hook_calls.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::dot_trigger {
namespace {

// The three events the dispatcher's blocks compare (the detonator's onspawnedremote, ondeposited, ontypechanged).
constexpr std::uint64_t kOnSpawnedRemote = 0x0360ce7dfc51dd43ULL;
constexpr std::uint64_t kOnDeposited = 0x088725a83299f252ULL;
constexpr std::uint64_t kOnTypeChanged = 0x71b428190a37733eULL;

using DispatchFn = std::uint64_t (*)(void* first, std::uint64_t* event);
using NextNodeFn = std::uint64_t (*)(std::uint64_t node, std::uint64_t event);

DispatchFn g_dispatch = nullptr;
NextNodeFn g_next_node = nullptr;
std::atomic_bool g_registered{false};

// The first argument and the return value pass through: the detour does not change what the dispatcher does with them.
std::uint64_t DispatchHook(void* first, std::uint64_t* event) {
  NEVR_SCRIPT_HOOK_CALL("DOT trigger dispatch");
  const std::uint64_t seen = EventForOriginal(*event);
  if (seen == *event) return g_dispatch(first, event);
  std::uint64_t replacement = seen;  // a local copy: the caller's event is not changed
  return g_dispatch(first, &replacement);
}

std::uint64_t NextNodeHook(std::uint64_t node, std::uint64_t event) {
  NEVR_SCRIPT_HOOK_CALL("DOT trigger resolve_next_node");
  return NextNode(node, event, g_next_node(node, event));
}

}  // namespace

bool IsTriggerEvent(std::uint64_t event) {
  return event == kOnSpawnedRemote || event == kOnDeposited || event == kOnTypeChanged;
}

std::uint64_t EventForOriginal(std::uint64_t event) { return IsTriggerEvent(event) ? kNoEvent : event; }

std::uint64_t NextNode(std::uint64_t node, std::uint64_t event, std::uint64_t stock) {
  for (const Row& row : kRows) {
    if (row.node != node) continue;
    if (row.event == event || (row.zero_event_too && event == 0)) return row.next;
  }
  return stock;
}

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kDispatchRva, kDispatchPrologue, sizeof kDispatchPrologue, reinterpret_cast<void*>(&DispatchHook),
       reinterpret_cast<void**>(&g_dispatch), "DOT trigger: the three events the original never sees"},
      {kScript, kNextNodeRva, kNextNodePrologue, sizeof kNextNodePrologue, reinterpret_cast<void*>(&NextNodeHook),
       reinterpret_cast<void**>(&g_next_node), "DOT trigger: the next-node table"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] DOT trigger fix registered script=%016llx hooks=2 (the trigger's three events never start their "
      "threads; the detonator copy's state changes go to the release-trigger node or stop; installed when the copy loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() { g_registered = false; }
#endif

}  // namespace nevr_combat::dot_trigger
