#include "runtime/combat/team_ready.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::team_ready {
namespace {

using ValueFn = bool (*)(void* ctx);
using ShouldExecuteFn = std::uint64_t (*)(void* ctx);

ValueFn g_blue_value = nullptr;  // trampolines MinHook fills; the replacements never call them
ValueFn g_red_value = nullptr;
ShouldExecuteFn g_blue_should = nullptr;
ShouldExecuteFn g_red_should = nullptr;
void* g_accessor = nullptr;      // the engine's function the slot held
void* g_module = nullptr;        // the script's base while the slot is replaced
std::atomic_bool g_registered{false};

thread_local int t_should_execute = 0;  // how deep this thread is inside a team-ready shouldexecute

bool ValueTrue(void*) { return true; }

std::uint64_t BlueShouldExecute(void* ctx) {
  ++t_should_execute;
  const std::uint64_t r = g_blue_should(ctx);
  --t_should_execute;
  return r;
}

std::uint64_t RedShouldExecute(void* ctx) {
  ++t_should_execute;
  const std::uint64_t r = g_red_should(ctx);
  --t_should_execute;
  return r;
}

std::uint64_t Accessor(void* ctx, std::uint64_t field, void* out) {
  void* module = g_module;
  std::uint64_t demo = 0;
  if (module != nullptr) std::memcpy(&demo, static_cast<std::uint8_t*>(module) + kDemoFieldIdRva, sizeof demo);
  return ReadField(reinterpret_cast<AccessorFn>(g_accessor), demo, t_should_execute > 0, ctx, field, out);
}

}  // namespace

std::uint64_t ReadField(AccessorFn engine, std::uint64_t demo_field, bool in_should_execute, void* ctx,
                        std::uint64_t field, void* out) {
  const std::uint64_t r = engine(ctx, field, out);
  if (in_should_execute && field == demo_field && out != nullptr) *static_cast<std::uint8_t*>(out) = 0;
  return r;
}

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kBlueValueRva, kValuePrologue, sizeof kValuePrologue, reinterpret_cast<void*>(&ValueTrue),
       reinterpret_cast<void**>(&g_blue_value), "team ready: blue value (always true)"},
      {kScript, kRedValueRva, kValuePrologue, sizeof kValuePrologue, reinterpret_cast<void*>(&ValueTrue),
       reinterpret_cast<void**>(&g_red_value), "team ready: red value (always true)"},
      {kScript, kBlueShouldExecuteRva, kShouldExecutePrologue, sizeof kShouldExecutePrologue,
       reinterpret_cast<void*>(&BlueShouldExecute), reinterpret_cast<void**>(&g_blue_should),
       "team ready: blue shouldexecute (demo senders count)"},
      {kScript, kRedShouldExecuteRva, kShouldExecutePrologue, sizeof kShouldExecutePrologue,
       reinterpret_cast<void*>(&RedShouldExecute), reinterpret_cast<void**>(&g_red_should),
       "team ready: red shouldexecute (demo senders count)"},
  };
  const nevr_script_hooks::Slot slots[] = {
      {kScript, kAccessorSlotRva, reinterpret_cast<void*>(&Accessor), &g_accessor, &g_module,
       "team ready: field accessor (a demo sender reads as not a demo inside the shouldexecutes)"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  nevr_script_hooks::Register(slots, sizeof slots / sizeof slots[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] team ready fix registered script=%016llx hooks=4 slots=1 (TEAM READY sets the team's flag instead of "
      "flipping it, and a demo account can ready its team; installed when the Combat game-mode script loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_accessor = nullptr;
  g_module = nullptr;
}
#endif

}  // namespace nevr_combat::team_ready
