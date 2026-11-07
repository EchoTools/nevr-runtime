#include "runtime/combat/point_rate.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::point_rate {
namespace {

using ExpressionFn = float (*)(void* node);

constexpr std::size_t kTeamCount = sizeof kTeams / sizeof kTeams[0];

ExpressionFn g_original[kTeamCount] = {};
void* g_accessor = nullptr;            // the engine's function the slot held
std::atomic<void*> g_module{nullptr};  // the script's base while the slot is replaced
std::atomic<volatile std::uint8_t*> g_mailbox{nullptr};
std::atomic_bool g_registered{false};

// While a wrapper runs its expression: whether the answer was read, and the qword the engine wrote for it.
thread_local int t_in_expression = 0;
thread_local bool t_recorded = false;
thread_local std::uint64_t t_answer = 0;

struct ExpressionScope {
  ExpressionScope() {
    ++t_in_expression;
    t_recorded = false;
  }
  ~ExpressionScope() { --t_in_expression; }
  ExpressionScope(const ExpressionScope&) = delete;
  ExpressionScope& operator=(const ExpressionScope&) = delete;
};

template <typename T>
T Read(const std::uint8_t* at) {
  T v{};
  std::memcpy(&v, at, sizeof v);
  return v;
}

template <std::size_t I>
float Expression(void* node) {
  const rules::RulesState rules(g_mailbox.load(std::memory_order_relaxed));
  const rules::Pause paused = rules.Paused();
  const std::uint8_t rate = rules.PointRate();
  if (!Acts(paused, rate)) return g_original[I](node);
  float stock = 0;
  {
    const ExpressionScope scope;
    stock = g_original[I](node);
  }
  if (!t_recorded) return stock;  // the answer was not read: the game's value stands
  // node -> [node] the context (qword) -> [context+0x68] the globals (qword)
  const std::uint8_t* context = Read<const std::uint8_t*>(static_cast<const std::uint8_t*>(node));
  const std::uint8_t* globals = Read<const std::uint8_t*>(context + kContextGlobals);
  const float dt = Dt(Read<std::int64_t>(context + kContextDt), paused, rate);
  return Rise(Read<float>(globals + kTeams[I].charge), dt, t_answer == kTeams[I].answer, Read<float>(globals + kRateE4),
              Read<std::uint8_t>(globals + kRateE0On) != 0, Read<float>(globals + kRateE0),
              Read<float>(globals + kRequired));
}

std::uint64_t Accessor(void* ctx, std::uint64_t field, void* out) {
  const std::uint64_t r = reinterpret_cast<AccessorFn>(g_accessor)(ctx, field, out);
  if (t_in_expression > 0 && out != nullptr) {
    void* module = g_module.load();
    std::uint32_t answer_field = 0;
    if (module != nullptr) {
      std::memcpy(&answer_field, static_cast<std::uint8_t*>(module) + kAnswerFieldIdRva, sizeof answer_field);
    }
    if (module != nullptr && static_cast<std::uint32_t>(field) == answer_field) {  // the id is a dword
      std::memcpy(&t_answer, out, sizeof t_answer);  // a qword: compared in full (`cmp [rsp+0x28], rax`)
      t_recorded = true;
    }
  }
  return r;
}

}  // namespace

bool Acts(rules::Pause paused, std::uint8_t rate) { return paused != rules::Pause::kOff || rate != 0; }

float Dt(std::int64_t dt_us, rules::Pause paused, std::uint8_t rate) {
  if (paused != rules::Pause::kOff) return 0.0f;           // his `xorps dt, dt`
  float dt = static_cast<float>(dt_us) / 1e6f;               // cvtsi2ss, divss [0x2A1D8]
  if (rate != 0) dt *= static_cast<float>(rate) * 0.25f;     // cvtsi2ss xmm3, rate; mulss [0.25]; mulss dt, xmm3
  return dt;
}

float Rise(float charge, float dt, bool answer_is_team, float rate_e4, bool rate_e0_on, float rate_e0,
           float required) {
  const float k1 = answer_is_team ? 1.0f : rate_e4;
  const float k2 = rate_e0_on ? rate_e0 : 1.0f;
  const float r = (dt * k1) * k2 + charge;
  if (r > required) return required;  // comiss r, required; ja
  if (0.0f > r) return 0.0f;          // comiss 0, r: below 0
  return r;
}

void Register(volatile std::uint8_t* mailbox) {
  if (g_registered.exchange(true)) return;
  g_mailbox.store(mailbox);
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kTeams[0].rva, kTeams[0].expected, kTeams[0].expected_len, reinterpret_cast<void*>(&Expression<0>),
       reinterpret_cast<void**>(&g_original[0]), kTeams[0].name},
      {kScript, kTeams[1].rva, kTeams[1].expected, kTeams[1].expected_len, reinterpret_cast<void*>(&Expression<1>),
       reinterpret_cast<void**>(&g_original[1]), kTeams[1].name},
  };
  static_assert(sizeof rows / sizeof rows[0] == kTeamCount);
  const nevr_script_hooks::Slot slots[] = {
      {kScript, kAccessorSlotRva, reinterpret_cast<void*>(&Accessor), &g_accessor, &g_module,
       "point rate: field accessor (records the answer the points expressions ask for)"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  nevr_script_hooks::Register(slots, sizeof slots / sizeof slots[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] point rate fix registered script=%016llx hooks=2 slots=1 (a capture point's points rise at the "
      "RULES page's POINT RATE and stand still while PAUSE is on; installed when the capture point script loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_accessor = nullptr;
  g_module.store(nullptr);
  g_mailbox.store(nullptr);
  for (ExpressionFn& original : g_original) original = nullptr;
}
#endif

}  // namespace nevr_combat::point_rate
