#include "runtime/combat/rules_button.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::rules_button {
namespace {

// A module base is 64 KiB aligned, so bit 0 of the base a slot holds is free. The replacement sets it at its first
// write after a load; the table stores the plain base on each install, which clears it, so each load logs once.
constexpr std::uintptr_t kFired = 1;

struct FlagSlot {
  const char* field;      // the flag's name, for the log
  std::uint32_t id_rva;   // where setup_bindings stored the field's id (a dword)
  std::uint8_t value;     // what the podium reads
  void* engine;           // the engine's accessor the slot held
  std::atomic<void*> module;  // the script's base while the slot is replaced (bit 0: fired on this load)
};

FlagSlot g_arena{"matchisarena", kArenaFieldIdRva, 1, nullptr, {nullptr}};
FlagSlot g_combat{"matchiscombat", kCombatFieldIdRva, 0, nullptr, {nullptr}};
std::atomic_bool g_registered{false};

std::uint64_t Accessor(FlagSlot& s, void* ctx, std::uint64_t field, void* out) {
  const auto engine = reinterpret_cast<AccessorFn>(s.engine);
  void* held = s.module.load();
  const auto bits = reinterpret_cast<std::uintptr_t>(held);
  auto* base = reinterpret_cast<std::uint8_t*>(bits & ~kFired);
  // No script, so no field id to compare: the engine's call only. Comparing with an id of 0 would act on field 0.
  if (base == nullptr) return engine(ctx, field, out);
  std::uint32_t id = 0;
  std::memcpy(&id, base + s.id_rva, sizeof id);
  int engine_byte = -1;
  const std::uint64_t r = ReadFlag(engine, id, s.value, ctx, field, out, &engine_byte);
  if (engine_byte >= 0 && (bits & kFired) == 0 &&
      s.module.compare_exchange_strong(held, reinterpret_cast<void*>(bits | kFired))) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.COMBAT] rules button: first read on this load field=%s engine=%d written=%u script=%016llx base=%p "
        "(the podium reads Arena, not Combat, so its RULES button is pressable; the match stays Combat)",
        s.field, engine_byte, static_cast<unsigned>(s.value), static_cast<unsigned long long>(kScript),
        static_cast<void*>(base));
  }
  return r;
}

std::uint64_t ArenaAccessor(void* ctx, std::uint64_t field, void* out) { return Accessor(g_arena, ctx, field, out); }
std::uint64_t CombatAccessor(void* ctx, std::uint64_t field, void* out) { return Accessor(g_combat, ctx, field, out); }

}  // namespace

std::uint64_t ReadFlag(AccessorFn engine, std::uint32_t field_id, std::uint8_t value, void* ctx, std::uint64_t field,
                       void* out, int* engine_byte) {
  const std::uint64_t r = engine(ctx, field, out);
  if (out != nullptr && static_cast<std::uint32_t>(field) == field_id) {
    auto* byte = static_cast<std::uint8_t*>(out);
    if (engine_byte != nullptr) *engine_byte = *byte;
    *byte = value;  // one byte: a one-byte flag, in the data or on the caller's stack
  }
  return r;
}

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Slot slots[] = {
      {kScript, kArenaSlotRva, reinterpret_cast<void*>(&ArenaAccessor), &g_arena.engine, &g_arena.module,
       "rules button: matchisarena reads 1 on the podium"},
      {kScript, kCombatSlotRva, reinterpret_cast<void*>(&CombatAccessor), &g_combat.engine, &g_combat.module,
       "rules button: matchiscombat reads 0 on the podium"},
  };
  nevr_script_hooks::Register(slots, sizeof slots / sizeof slots[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] rules button fix registered script=%016llx hooks=0 slots=2 (the podium terminal reads its match as "
      "Arena, so its RULES button is pressable in Combat private matches; the match itself stays Combat; installed when "
      "the podium script loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  for (FlagSlot* s : {&g_arena, &g_combat}) {
    s->engine = nullptr;
    s->module.store(nullptr);
  }
}

void ForgetModulesForTest() {
  g_arena.module.store(nullptr);
  g_combat.module.store(nullptr);
}
#endif

}  // namespace nevr_combat::rules_button
