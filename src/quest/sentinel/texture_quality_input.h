/* The texture-quality owner's one input from combat (#1737, ADR 0039; docs/quest/combat/texture_quest2.md): where the game is, for the Quest 2
 * step-down (texture_quality_rules.h).
 *
 * texture_quality owns the eye size; it does not include a combat header. Combat's `texture_quest2` row registers a function that returns its
 * last look (texture_quality_rules.h `Look`) and the step-down switches. No registration (the release APK, or the key false) leaves the owner's
 * behaviour exactly as it was: the per-frame apply reads one null pointer.
 *
 * The look function is called on the owner's poller thread and on the render thread right before a look-driven Resize: it returns published
 * atomics only (no game read, no lock, no log, no allocation).
 *
 * Header only: an inline atomic is one object across the sentinel and the combat translation units.
 */
#pragma once

#include <atomic>

#include "texture_quality_rules.h"

namespace nevr_quest::texture_quality::input {

using LookFn = rules::Look (*)() noexcept;

namespace detail {
inline std::atomic<LookFn> g_look{nullptr};
inline std::atomic<bool> g_lobby{true};
}  // namespace detail

// Registers combat's look and its switches. The switches are stored before the function, so a reader that sees the function sees them.
inline void RegisterStepDown(LookFn look, rules::StepDownSwitches switches) noexcept {
  detail::g_lobby.store(switches.lobby, std::memory_order_relaxed);
  detail::g_look.store(look, std::memory_order_release);
}

// The registered look function, or nullptr.
inline LookFn Registered() noexcept { return detail::g_look.load(std::memory_order_acquire); }

inline rules::StepDownSwitches Switches() noexcept {
  rules::StepDownSwitches switches;
  switches.lobby = detail::g_lobby.load(std::memory_order_relaxed);
  return switches;
}

// Test support: forgets the registration.
inline void ResetForTest() noexcept {
  detail::g_look.store(nullptr, std::memory_order_release);
  detail::g_lobby.store(true, std::memory_order_relaxed);
}

}  // namespace nevr_quest::texture_quality::input
