#include "runtime/combat/unlock_time.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::unlock_time {
namespace {

using NodeFn = std::uint64_t (*)(void* node);
using ExpressionFn = float (*)(void* node);
using FourFn = std::uint64_t (*)(void* a, std::uint64_t b, std::uint64_t c, void* d);

NodeFn g_countdown = nullptr;
ExpressionFn g_lock_left = nullptr;
FourFn g_getter = nullptr;
FourFn g_state_pack = nullptr;
FourFn g_typed_getter = nullptr;
std::atomic<volatile std::uint8_t*> g_mailbox{nullptr};
std::atomic_bool g_registered{false};

rules::RulesState Rules() { return rules::RulesState(g_mailbox.load(std::memory_order_relaxed)); }

template <typename T>
T Read(const std::uint8_t* at) {
  T v{};
  std::memcpy(&v, at, sizeof v);
  return v;
}
template <typename T>
void Write(std::uint8_t* at, T v) {
  std::memcpy(at, &v, sizeof v);
}

std::uint8_t* GlobalsOfContext(const void* context) {
  return Read<std::uint8_t*>(static_cast<const std::uint8_t*>(context) + kContextGlobals);
}
const std::uint8_t* ContextOfNode(const void* node) {
  return Read<const std::uint8_t*>(static_cast<const std::uint8_t*>(node));
}

// While T is set: the lock time is the page's for the duration of one call, and the map's own after it.
class LockSwap {
 public:
  explicit LockSwap(std::uint8_t* globals) : at_(globals + kLockTime), own_(Read<float>(at_)) {
    Write(at_, EffectiveLock(Rules(), own_));
  }
  ~LockSwap() { Write(at_, own_); }
  LockSwap(const LockSwap&) = delete;
  LockSwap& operator=(const LockSwap&) = delete;

 private:
  std::uint8_t* at_;
  float own_;
};

std::uint64_t Countdown(void* node) {
  const std::uint8_t* context = ContextOfNode(node);
  std::uint8_t* globals = GlobalsOfContext(context);
  if (Rules().Paused() == rules::Pause::kOff) {
    const LockSwap swap(globals);
    return g_countdown(node);
  }
  // PAUSE: the stock node's elapsed time, then his freeze, then the stock node's write.
  if (static_cast<const std::uint8_t*>(node)[kNodeNoWrite] != 0) return ~std::uint64_t{0};
  const float start = Read<float>(globals + kCountdownStart);
  const float elapsed = static_cast<float>(Read<std::int64_t>(context + kContextNow)) / 1e6f - start;
  const Frozen f = Freeze(EffectiveLock(Rules(), Read<float>(globals + kLockTime)), elapsed, start,
                          Read<float>(globals + kCountdown));
  Write(globals + kCountdownStart, f.start);
  Write(globals + kCountdown, f.countdown);
  return 0;
}

float LockLeft(void* node) {
  const LockSwap swap(GlobalsOfContext(ContextOfNode(node)));
  return g_lock_left(node);
}

std::uint64_t Getter(void* context, std::uint64_t name, std::uint64_t type, void* out) {
  const LockSwap swap(GlobalsOfContext(context));
  return g_getter(context, name, type, out);
}

std::uint64_t StatePack(void* context, std::uint64_t out, std::uint64_t a3, void* a4) {
  const LockSwap swap(GlobalsOfContext(context));
  return g_state_pack(context, out, a3, a4);
}

std::uint64_t TypedGetter(void* object, std::uint64_t name, std::uint64_t type, void* out) {
  // Only the path that reads the lock time takes the object as the globals.
  if (static_cast<std::uint32_t>(type) != kTypedGetterLockType || name != kTypedGetterLockName) {
    return g_typed_getter(object, name, type, out);
  }
  const LockSwap swap(static_cast<std::uint8_t*>(object));
  return g_typed_getter(object, name, type, out);
}

}  // namespace

float EffectiveLock(const rules::RulesState& rules, float own) {
  const float total = rules.RoundTotal();
  // `cvttss2si eax, [T]; test eax, eax; jle keep`: an out-of-range T converts to INT_MIN, which keeps the map's own.
  if (!(total >= 1.0f && total < 2147483648.0f)) return own;
  const std::int32_t t = static_cast<std::int32_t>(total);
  return static_cast<float>(t / 60);  // idiv: toward zero
}

Frozen Freeze(float lock, float elapsed, float start, float countdown) {
  const float now = elapsed + start;    // addss xmm2, [start]
  const float x1 = lock - countdown;    // subss xmm1, [countdown]
  return Frozen{now - x1, lock - x1};   // the new start; the stock node's lock - x1
}

void Register(volatile std::uint8_t* mailbox) {
  if (g_registered.exchange(true)) return;
  g_mailbox.store(mailbox);
  const nevr_script_hooks::Hook rows[] = {
      {kScript, 0x10030, kCountdownNode, sizeof kCountdownNode, reinterpret_cast<void*>(&Countdown),
       reinterpret_cast<void**>(&g_countdown), "unlock time: the unlock countdown"},
      {kScript, 0x1F130, kLockLeftExpression, sizeof kLockLeftExpression, reinterpret_cast<void*>(&LockLeft),
       reinterpret_cast<void**>(&g_lock_left), "unlock time: time left to unlock"},
      {kScript, 0x178F0, kGetterHead, sizeof kGetterHead, reinterpret_cast<void*>(&Getter),
       reinterpret_cast<void**>(&g_getter), "unlock time: get-by-name"},
      {kScript, 0x179CF, kGetterLockRead, sizeof kGetterLockRead, nullptr, nullptr,
       "unlock time: pin, get-by-name reads the lock time"},
      {kScript, 0x8AC0, kStatePackHead, sizeof kStatePackHead, reinterpret_cast<void*>(&StatePack),
       reinterpret_cast<void**>(&g_state_pack), "unlock time: the state pack"},
      {kScript, 0x91D4, kStatePackLockRead, sizeof kStatePackLockRead, nullptr, nullptr,
       "unlock time: pin, the state pack reads the lock time"},
      {kScript, 0x4BC0, kTypedGetterHead, sizeof kTypedGetterHead, reinterpret_cast<void*>(&TypedGetter),
       reinterpret_cast<void**>(&g_typed_getter), "unlock time: the typed getter"},
      {kScript, 0x4CCB, kTypedGetterLockRead, sizeof kTypedGetterLockRead, nullptr, nullptr,
       "unlock time: pin, the typed getter reads the lock time"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] unlock time fix registered script=%016llx hooks=5 pins=3 (in Combat the capture point unlocks at "
      "the RULES page's unlock time, and its unlock countdown stands still while PAUSE is on; installed when the capture "
      "point script loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_mailbox.store(nullptr);
  g_countdown = nullptr;
  g_lock_left = nullptr;
  g_getter = g_state_pack = g_typed_getter = nullptr;
}
#endif

}  // namespace nevr_combat::unlock_time
