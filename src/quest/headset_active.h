/* The headset profile the identity step chose (ADR 0031 decision 4, #2037): the one place the rest of the Quest runtime reads it from.
 *
 * The identity step (integration/headset_identity_step.cpp) names the headset once at start, from the headset's system properties, and
 * stores its profile here. Every consumer of a per-headset setting reads it here and never reads the identity inputs itself: the texture
 * hook and the texture menu (through texture_quality_hook.h's ActiveHeadsetProfile), the heap sizes (combat/heaps.cpp) and the refresh-rate
 * rule (combat/refresh_rate.cpp).
 *
 * Null until the step runs. The sentinel's constructor sequence runs the step before libr15 is attached and before the combat early step
 * (integration/ctor_sequence.cpp; integration_sequence_test.cpp pins the order), so a consumer that finds no profile is a sequence that did
 * not run the step: it takes the generic profile (`ActiveOrGeneric`) and says so in a Warning of its own.
 *
 * Threads: the step stores on the constructor thread; heaps reads on the same thread, refresh_rate on the post-load thread, the texture
 * hook on the render thread. A release store and an acquire load are enough: the profile itself is constant (headset_profile.h).
 *
 * Header only, no Android or game header: an inline atomic is one object across the sentinel, the combat translation units and the host
 * tests (the precedent is sentinel/texture_quality_input.h).
 */
#pragma once

#include <atomic>

#include "quest/headset_profile.h"

namespace nevr_quest::headset_active {

namespace detail {
inline std::atomic<const headset_profile::Profile*> g_active{nullptr};
}  // namespace detail

// The identity step's one write (a test's too). nullptr forgets the profile.
inline void SetActive(const headset_profile::Profile* profile) noexcept { detail::g_active.store(profile, std::memory_order_release); }

// The profile the step chose, or nullptr before the step ran.
inline const headset_profile::Profile* Active() noexcept { return detail::g_active.load(std::memory_order_acquire); }

// The profile the step chose, or the generic profile when none was set. Never null.
inline const headset_profile::Profile* ActiveOrGeneric() noexcept {
  const headset_profile::Profile* const profile = Active();
  return profile != nullptr ? profile : &headset_profile::kGeneric;
}

}  // namespace nevr_quest::headset_active
