// The one readiness the Quest login path agrees on (#239, #581). Token auth publishes it; two things read it:
//
//   * the login prerequisites (IdentitySource::Ready and IdentitySource::HoldsAnswers, on the game's OVR message
//     pump): a real NEVR login may stand in for the Oculus answers only while the gate is ready and the current
//     attempt is not poisoned, and the four local answers are held back only while the gate says a token is coming
//     without the player;
//   * the sign-in prompt's page-enable hook (login_prompt_hook.cpp, possibly on a worker thread): while the
//     gate is not ready it keeps the logging-in page from replacing the screen that shows the code, and it
//     poisons the attempt it did that for.
//
// Three bits in one word:
//   kReadyBit    token auth can answer a login now (Fetch would return Ok).
//   kPoisonedBit the current attempt had its logging-in page skipped: it fails its prerequisites until it ends.
//   kHoldBit     token auth will produce a token without the player (starting, refreshing, expired, a failure it
//                retries, a token that ran out), so the login answers wait for it. It is clear while the player is
//                awaited and when no token will come: the game then gets its answers, finds the stand-ins reset
//                (the identity is not ready) and fails the login on its own path, which is the screen the sign-in
//                code is written onto (login_prompt_hook.cpp, IsLocalLoginFailure).
//
// Why one word and a poison bit. Two decisions reading two flags the token-auth poll sets separately would
// let a RETRY in the gap get its logging-in page skipped while the login itself went on to succeed,
// stranding the player on the login-failed screen while logged in. With one atomic the skip stops in the
// same instant the login may proceed, and an attempt whose page was skipped fails its
// prerequisites for as long as that attempt runs (the sentinel clears the poison when the game leaves
// "logging in"), even if the gate turns ready meanwhile. Ready and hold are published together (Publish) for
// the same reason: a reader never sees one of a snapshot's two answers with the other from the snapshot before.
// A poisoned attempt never holds: its answers are delivered and the attempt fails.
//
// Constant-initialised inline atomic: no static initialiser, safe to read from any thread, lock-free.
#pragma once

#include <atomic>
#include <cstdint>

namespace nevr_quest_login::attempt_gate {

inline constexpr std::uint32_t kReadyBit = 1;
inline constexpr std::uint32_t kPoisonedBit = 2;
inline constexpr std::uint32_t kHoldBit = 4;

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
inline bool LoginMayProceed() noexcept {
  return (g_state.load(std::memory_order_acquire) & (kReadyBit | kPoisonedBit)) == kReadyBit;
}

// Publishes one token-auth observation: the ready and hold bits are set together in one compare-and-swap loop and
// a poison stays as it is. Token auth's state change is one store for both readers.
inline void Publish(bool ready, bool hold) noexcept {
  const std::uint32_t bits = (ready ? kReadyBit : 0U) | (hold ? kHoldBit : 0U);
  std::uint32_t seen = g_state.load(std::memory_order_acquire);
  while (!g_state.compare_exchange_weak(seen, (seen & kPoisonedBit) | bits, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
  }
}

// The login answers wait for token auth: it has a token coming without the player, this attempt is not poisoned
// and the gate is not ready. One atomic load.
inline bool HoldsAnswers() noexcept {
  return (g_state.load(std::memory_order_acquire) & (kHoldBit | kReadyBit | kPoisonedBit)) == kHoldBit;
}

inline void Reset() noexcept { g_state.store(0, std::memory_order_release); }

}  // namespace nevr_quest_login::attempt_gate
