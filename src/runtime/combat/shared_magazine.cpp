#include "runtime/combat/shared_magazine.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::shared_magazine {
namespace {

using ConditionFn = bool (*)(void* thread);

ConditionFn g_condition = nullptr;
void* g_accessor = nullptr;            // the engine's function the slot held
std::atomic<void*> g_module{nullptr};  // the script's base while the slot is replaced
std::atomic_bool g_registered{false};

thread_local int t_in_condition = 0;
thread_local bool t_type_seen = false;
thread_local std::uint64_t t_type = 0;

// Marks this thread as inside the condition, and forgets what an earlier one recorded.
struct ConditionScope {
  ConditionScope() {
    ++t_in_condition;
    t_type_seen = false;
  }
  ~ConditionScope() { --t_in_condition; }
  ConditionScope(const ConditionScope&) = delete;
  ConditionScope& operator=(const ConditionScope&) = delete;
};

bool ConditionHook(void* thread) {
  bool stock = false;
  {
    const ConditionScope scope;
    stock = g_condition(thread);
  }
  std::uint64_t type = 0;
  const bool seen = RecordedType(&type);
  return Enables(stock, seen, type);
}

std::uint64_t Accessor(void* ctx, std::uint64_t field, void* out) {
  void* module = g_module.load();
  std::uint32_t type_field = 0;
  if (module != nullptr) std::memcpy(&type_field, static_cast<std::uint8_t*>(module) + kTypeFieldIdRva, sizeof type_field);
  return ReadField(reinterpret_cast<AccessorFn>(g_accessor), type_field, t_in_condition > 0, ctx, field, out);
}

}  // namespace

bool Enables(bool stock, bool type_seen, std::uint64_t type) {
  if (!stock || !type_seen) return stock;
  return type != kDot && type != kDrone;
}

std::uint64_t ReadField(AccessorFn engine, std::uint32_t type_field, bool in_condition, void* ctx, std::uint64_t field,
                        void* out) {
  const std::uint64_t r = engine(ctx, field, out);
  if (in_condition && static_cast<std::uint32_t>(field) == type_field && out != nullptr) {
    std::memcpy(&t_type, out, sizeof t_type);  // a qword: the condition compares it in full
    t_type_seen = true;
  }
  return r;
}

bool RecordedType(std::uint64_t* type) {
  *type = t_type;
  return t_type_seen;
}

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kConditionRva, kConditionPrologue, sizeof kConditionPrologue, reinterpret_cast<void*>(&ConditionHook),
       reinterpret_cast<void**>(&g_condition), "shared magazine: no frisbee for the dot or the drone"},
  };
  const nevr_script_hooks::Slot slots[] = {
      {kScript, kAccessorSlotRva, reinterpret_cast<void*>(&Accessor), &g_accessor, &g_module,
       "shared magazine: field accessor (records the magazine type inside the frisbee condition)"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  nevr_script_hooks::Register(slots, sizeof slots / sizeof slots[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] shared magazine fix registered script=%016llx hooks=1 slots=1 (a thrown DOT FIELD or drone no longer "
      "asks for a frisbee component it does not have; installed when the shared magazine script loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_accessor = nullptr;
  g_module.store(nullptr);
  t_type_seen = false;
  t_type = 0;
}
#endif

}  // namespace nevr_combat::shared_magazine
