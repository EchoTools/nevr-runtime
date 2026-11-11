#include "quest/combat/combat_active.h"

#include <atomic>

#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat {

namespace {

// Plain atomics of enum values, constant-initialised: the sentinel has no load-time initializer to run (the static-init check).
std::atomic<int> g_variant{static_cast<int>(build_variant::Variant::kNone)};
std::atomic<int> g_state{0};  // 0: not resolved; 1: resolved
std::atomic<int> g_active{0};
std::atomic<int> g_dev{0};
std::atomic<int> g_reason{static_cast<int>(Reason::kUnresolved)};

}  // namespace

const char* ReasonName(Reason reason) noexcept {
  switch (reason) {
    case Reason::kUnresolved: return "unresolved";
    case Reason::kVariantCarriesCombat: return "variant";
    case Reason::kForced: return "forced";
    case Reason::kNotACombatVariant: return "not_a_combat_variant";
  }
  return "unresolved";
}

const char* ForceName(ForceOutcome outcome) noexcept {
  switch (outcome) {
    case ForceOutcome::kNotAsked: return "not_asked";
    case ForceOutcome::kHonoured: return "honoured";
    case ForceOutcome::kRedundant: return "redundant";
    case ForceOutcome::kRefused: return "refused";
  }
  return "not_asked";
}

Decision Decide(build_variant::Variant variant, bool devBuild, bool forceAsked) noexcept {
  Decision decision;
  const bool carries = build_variant::RunsFoldedCombat(variant);
  decision.active = carries;
  decision.reason = carries ? Reason::kVariantCarriesCombat : Reason::kNotACombatVariant;
  decision.force = ForceOutcome::kNotAsked;
  if (!forceAsked) return decision;
  if (!devBuild) {
    decision.force = ForceOutcome::kRefused;  // a release build never takes the request; the variant alone decides
    return decision;
  }
  if (carries) {
    decision.force = ForceOutcome::kRedundant;
    return decision;
  }
  decision.active = true;
  decision.reason = Reason::kForced;
  decision.force = ForceOutcome::kHonoured;
  return decision;
}

void SetVariant(build_variant::Variant variant) noexcept {
  if (g_state.load(std::memory_order_acquire) != 0) return;
  g_variant.store(static_cast<int>(variant), std::memory_order_release);
}

void Resolve(bool devBuild, bool forceAsked) noexcept {
  int expected = 0;
  if (!g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
  const build_variant::Variant variant = static_cast<build_variant::Variant>(g_variant.load(std::memory_order_acquire));
  const Decision decision = Decide(variant, devBuild, forceAsked);
  g_dev.store(devBuild ? 1 : 0, std::memory_order_release);
  g_active.store(decision.active ? 1 : 0, std::memory_order_release);
  g_reason.store(static_cast<int>(decision.reason), std::memory_order_release);
  const char* consequence =
      decision.active ? "folded combat modules install in this run"
                      : "folded combat modules do not install in this run; the game keeps its own behaviour";
  if (decision.force == ForceOutcome::kRefused) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_active",
                        {{"variant", build_variant::VariantName(variant)}, {"kind", devBuild ? "dev" : "release"},
                         {"active", decision.active ? 1 : 0}, {"reason", ReasonName(decision.reason)},
                         {"force", ForceName(decision.force)},
                         {"msg", decision.active
                                     ? "combat.force is honoured in development builds only and is ignored here; folded combat modules install in this run because of the variant"
                                     : "combat.force is honoured in development builds only and is ignored here; folded combat modules do not install in this run"}});
    return;
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_active",
                      {{"variant", build_variant::VariantName(variant)}, {"kind", devBuild ? "dev" : "release"},
                       {"active", decision.active ? 1 : 0}, {"reason", ReasonName(decision.reason)},
                       {"force", ForceName(decision.force)}, {"msg", consequence}});
}

build_variant::Variant RecordedVariant() noexcept {
  return static_cast<build_variant::Variant>(g_variant.load(std::memory_order_acquire));
}

bool Active() noexcept { return g_active.load(std::memory_order_acquire) != 0; }

bool DevBuild() noexcept { return g_dev.load(std::memory_order_acquire) != 0; }

Reason ActiveReason() noexcept { return static_cast<Reason>(g_reason.load(std::memory_order_acquire)); }

void ResetForTest() noexcept {
  g_variant.store(static_cast<int>(build_variant::Variant::kNone), std::memory_order_release);
  g_active.store(0, std::memory_order_release);
  g_dev.store(0, std::memory_order_release);
  g_reason.store(static_cast<int>(Reason::kUnresolved), std::memory_order_release);
  g_state.store(0, std::memory_order_release);
}

}  // namespace nevr_quest::combat
