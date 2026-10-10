// The one readiness the Quest login path agrees on (#239). Token auth publishes it; two things read it:
//
//   * the login prerequisites (IdentitySource::Ready, on the game's OVR message pump): a real NEVR login
//     may stand in for the Oculus answers only while the gate is ready and the current attempt is not
//     poisoned;
//   * the sign-in prompt's page-enable hook (login_prompt_hook.cpp, possibly on a worker thread): while the
//     gate is not ready it keeps the logging-in page from replacing the screen that shows the code, and it
//     poisons the attempt it did that for.
//
// Why one word and a poison bit. The two decisions used to read two flags the token-auth poll set
// separately, so a RETRY in the gap could get its logging-in page skipped while the login itself went on to
// succeed, stranding the player on the login-failed screen while logged in. Now there is one atomic: the
// skip stops in the same instant the login may proceed, and an attempt whose page was skipped fails its
// prerequisites for as long as that attempt runs (the sentinel clears the poison when the game leaves
// "logging in"), even if the gate turns ready meanwhile.
//
// Constant-initialised inline atomic: no static initialiser, safe to read from any thread, lock-free.
#pragma once

#include <atomic>
#include <cstdint>

namespace QuestLogin::attempt_gate {

inline constexpr std::uint32_t kReadyBit = 1;
inline constexpr std::uint32_t kPoisonedBit = 2;

inline std::atomic<std::uint32_t> g_state{0};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "the attempt gate must be lock-free");

// Token auth can answer a login now (Fetch would return Ok).
inline void SetReady(bool ready) noexcept {
  if (ready) {
    g_state.fetch_or(kReadyBit, std::memory_order_acq_rel);
  } else {
    g_state.fetch_and(~kReadyBit, std::memory_order_acq_rel);
  }
}
inline bool IsReady() noexcept { return (g_state.load(std::memory_order_acquire) & kReadyBit) != 0; }

// The current login attempt had its logging-in page skipped: it must fail.
inline void Poison() noexcept { g_state.fetch_or(kPoisonedBit, std::memory_order_acq_rel); }

// Test seam: called between PoisonIfNotReady's read of the word and its compare-and-swap, to flip the
// readiness exactly there. Null in production.
inline void (*g_beforePoisonCas)() noexcept = nullptr;

// Poisons the attempt only if the gate is still not ready: one compare-and-swap on the word, so the decision
// to skip a logging-in page and the poison are a single step. False (nothing poisoned) when the gate is ready
// or turned ready meanwhile: the caller lets the page through, because a login that may proceed needs it.
inline bool PoisonIfNotReady() noexcept {
  std::uint32_t seen = g_state.load(std::memory_order_acquire);
  for (;;) {
    if ((seen & kReadyBit) != 0) return false;
    if (g_beforePoisonCas != nullptr) g_beforePoisonCas();
    if (g_state.compare_exchange_weak(seen, seen | kPoisonedBit, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
      return true;
    }
  }
}
inline void ClearPoison() noexcept { g_state.fetch_and(~kPoisonedBit, std::memory_order_acq_rel); }

// A login may stand in for the Oculus answers: ready, and the attempt is not poisoned. One atomic load.
inline bool LoginMayProceed() noexcept { return g_state.load(std::memory_order_acquire) == kReadyBit; }

inline void Reset() noexcept { g_state.store(0, std::memory_order_release); }

}  // namespace QuestLogin::attempt_gate
