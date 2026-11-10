#include "runtime/combat/dot_script.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::dot_script {
namespace {

using ExecuteFn = std::uint64_t (*)(void* node, void* ctx);
using NextNodeFn = std::uint64_t (*)(std::uint64_t node, std::uint64_t port);

ExecuteFn g_execute = nullptr;
NextNodeFn g_next_node = nullptr;
std::atomic_bool g_registered{false};

std::uint64_t PlaySoundExecute(void* node, void* ctx) {
  auto* at = static_cast<std::uint8_t*>(node) + kEventOffset;
  std::uint32_t event = 0;
  std::memcpy(&event, at, sizeof event);  // a dword: the builders write it with `mov dword [rbp-0x11], imm32`
  const std::uint32_t played = Event(event);
  if (played != event) std::memcpy(at, &played, sizeof played);
  return g_execute(node, ctx);
}

std::uint64_t NextNodeHook(std::uint64_t node, std::uint64_t port) { return NextNode(node, g_next_node(node, port)); }

}  // namespace

std::uint32_t Event(std::uint32_t stock) {
  if (stock == kFieldOnEvent) return kStunActivate;
  if (stock == kFieldStopEvent) return kStunStop;
  return stock;
}

std::uint64_t NextNode(std::uint64_t node, std::uint64_t stock) {
  return node == kSetupNode && stock == kFirstSceneSetNode ? kAfterSceneSetNodes : stock;
}

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kPlaySoundExecuteRva, kPlaySoundPrologue, sizeof kPlaySoundPrologue,
       reinterpret_cast<void*>(&PlaySoundExecute), reinterpret_cast<void**>(&g_execute),
       "DOT FIELD: the stun grenade's sound events"},
      {kScript, kNextNodeRva, kNextNodePrologue, sizeof kNextNodePrologue, reinterpret_cast<void*>(&NextNodeHook),
       reinterpret_cast<void**>(&g_next_node), "DOT FIELD: setup skips the three scene-set nodes"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] DOT FIELD fix registered script=%016llx hooks=2 (the field plays the stun grenade's sounds, and its "
      "setup no longer runs three scene-set nodes on a model it does not have; installed when the DOT's script loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() { g_registered = false; }
#endif

}  // namespace nevr_combat::dot_script
