#include "runtime/combat/orb_pause.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::orb_pause {
namespace {

using NodeFn = std::uint64_t (*)(void* thread);

NodeFn g_node = nullptr;
void* g_accessor = nullptr;            // the engine's function the slot held
std::atomic<void*> g_module{nullptr};  // the script's base while the slot is replaced
std::atomic<volatile std::uint8_t*> g_mailbox{nullptr};
std::atomic_bool g_registered{false};

thread_local int t_in_node = 0;

struct NodeScope {
  NodeScope() { ++t_in_node; }
  ~NodeScope() { --t_in_node; }
  NodeScope(const NodeScope&) = delete;
  NodeScope& operator=(const NodeScope&) = delete;
};

std::uint64_t NodeHook(void* thread) {
  const NodeScope scope;
  return g_node(thread);
}

std::uint64_t Accessor(void* ctx, std::uint64_t field, void* out) {
  void* module = g_module.load();
  std::uint32_t spawn_field = 0;
  if (module != nullptr) {
    std::memcpy(&spawn_field, static_cast<std::uint8_t*>(module) + kSpawnAccessFieldIdRva, sizeof spawn_field);
  }
  const rules::RulesState rules(g_mailbox.load());
  return ReadField(reinterpret_cast<AccessorFn>(g_accessor), spawn_field, t_in_node > 0, rules, ctx, field, out);
}

}  // namespace

bool Live(std::uint64_t spawn_access) { return spawn_access != kUnset && spawn_access != kPreMatch; }

std::uint64_t Acted(std::uint64_t spawn_access, rules::Pause paused) {
  return paused == rules::Pause::kOn ? kPreMatch : spawn_access;
}

std::uint64_t ReadField(AccessorFn engine, std::uint32_t spawn_field, bool in_node, const rules::RulesState& rules,
                        void* ctx, std::uint64_t field, void* out) {
  const std::uint64_t r = engine(ctx, field, out);
  if (in_node && static_cast<std::uint32_t>(field) == spawn_field && out != nullptr) {
    std::uint64_t value = 0;
    std::memcpy(&value, out, sizeof value);  // a qword: the node compares it in full (`cmp r8, -1`)
    rules.SetMatchLive(Live(value));
    const std::uint64_t acted = Acted(value, rules.Paused());
    if (acted != value) std::memcpy(out, &acted, sizeof acted);
  }
  return r;
}

void Register(volatile std::uint8_t* mailbox) {
  if (g_registered.exchange(true)) return;
  g_mailbox.store(mailbox);
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kNodeRva, kNodePrologue, sizeof kNodePrologue, reinterpret_cast<void*>(&NodeHook),
       reinterpret_cast<void**>(&g_node), "spawn orbs: the pre-match orbs while PAUSE is on"},
  };
  const nevr_script_hooks::Slot slots[] = {
      {kScript, kAccessorSlotRva, reinterpret_cast<void*>(&Accessor), &g_accessor, &g_module,
       "spawn orbs: field accessor (publishes match live, reads pre_match while paused)"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  nevr_script_hooks::Register(slots, sizeof slots / sizeof slots[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] orb pause fix registered script=%016llx hooks=1 slots=1 (while PAUSE is on the spawn orbs give the "
      "pre-match orbs, and the orbs say when the match is live for TEAM READY -> PAUSE; installed when the spawn point "
      "script loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_accessor = nullptr;
  g_module.store(nullptr);
  g_mailbox.store(nullptr);
}
#endif

}  // namespace nevr_combat::orb_pause
