#include "runtime/combat/rules_button.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::rules_button {
namespace {

void* g_arena_engine = nullptr;
void* g_combat_engine = nullptr;
std::atomic<void*> g_arena_module{nullptr};
std::atomic<void*> g_combat_module{nullptr};
std::atomic_bool g_registered{false};

std::uint32_t FieldId(const std::atomic<void*>& module, std::uint32_t rva) {
  void* base = module.load();
  std::uint32_t id = 0;
  if (base != nullptr) std::memcpy(&id, static_cast<std::uint8_t*>(base) + rva, sizeof id);
  return id;
}

std::uint64_t ArenaAccessor(void* ctx, std::uint64_t field, void* out) {
  return ReadFlag(reinterpret_cast<AccessorFn>(g_arena_engine), FieldId(g_arena_module, kArenaFieldIdRva), 1, ctx,
                  field, out);
}

std::uint64_t CombatAccessor(void* ctx, std::uint64_t field, void* out) {
  return ReadFlag(reinterpret_cast<AccessorFn>(g_combat_engine), FieldId(g_combat_module, kCombatFieldIdRva), 0, ctx,
                  field, out);
}

}  // namespace

std::uint64_t ReadFlag(AccessorFn engine, std::uint32_t field_id, std::uint8_t value, void* ctx, std::uint64_t field,
                       void* out) {
  const std::uint64_t r = engine(ctx, field, out);
  if (out != nullptr && static_cast<std::uint32_t>(field) == field_id) {
    *static_cast<std::uint8_t*>(out) = value;  // one byte: the two copies are adjacent bytes (+0x169, +0x16A)
  }
  return r;
}

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Slot slots[] = {
      {kScript, kArenaSlotRva, reinterpret_cast<void*>(&ArenaAccessor), &g_arena_engine, &g_arena_module,
       "rules button: matchisarena reads 1 on the podium"},
      {kScript, kCombatSlotRva, reinterpret_cast<void*>(&CombatAccessor), &g_combat_engine, &g_combat_module,
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
  g_arena_engine = g_combat_engine = nullptr;
  g_arena_module.store(nullptr);
  g_combat_module.store(nullptr);
}
#endif

}  // namespace nevr_combat::rules_button
