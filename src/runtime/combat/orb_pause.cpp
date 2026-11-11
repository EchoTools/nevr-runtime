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
// The last state each edge line reported: -1 none yet, 0, 1.
std::atomic<int> g_last_live{-1};
std::atomic<int> g_last_swap{-1};

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
  const auto engine = reinterpret_cast<AccessorFn>(g_accessor);
  void* module = g_module.load();
  // No script, so no field id to compare: the engine's call only. Comparing with an id of 0 would act on field 0.
  if (module == nullptr) return engine(ctx, field, out);
  std::uint32_t spawn_field = 0;
  std::memcpy(&spawn_field, static_cast<std::uint8_t*>(module) + kSpawnAccessFieldIdRva, sizeof spawn_field);
  const rules::RulesState rules(g_mailbox.load());
  return ReadField(engine, spawn_field, t_in_node > 0, rules, ctx, field, out);
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
    const bool live = Live(value);
    rules.SetMatchLive(live);
    const rules::Pause paused = rules.Paused();
    const std::uint64_t acted = Acted(value, paused);
    if (acted != value) std::memcpy(out, &acted, sizeof acted);
    const int was_live = g_last_live.exchange(live ? 1 : 0);
    if (was_live != (live ? 1 : 0)) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.COMBAT] orb pause: match live from=%s to=%d script=%016llx paused=%u (the RULES mailbox +0x2F, which "
          "VeilNet reads for TEAM READY -> PAUSE)",
          was_live < 0 ? "unset" : (was_live != 0 ? "1" : "0"), live ? 1 : 0, static_cast<unsigned long long>(kScript),
          static_cast<unsigned>(paused));
    }
    const int swap = acted != value ? 1 : 0;
    const int was_swap = g_last_swap.exchange(swap);
    if (was_swap != swap && (was_swap >= 0 || swap != 0)) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.COMBAT] orb pause: pre-match orbs %s script=%016llx paused=%u (%s)", swap != 0 ? "started" : "stopped",
          static_cast<unsigned long long>(kScript), static_cast<unsigned>(paused),
          swap != 0 ? "PAUSE is on: the spawn orbs give the players the pre-match orbs"
                    : "PAUSE is off or resuming: the spawn orbs give the round's orbs");
    }
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
  g_last_live = -1;
  g_last_swap = -1;
}

void ForgetModuleForTest() { g_module.store(nullptr); }
#endif

}  // namespace nevr_combat::orb_pause
