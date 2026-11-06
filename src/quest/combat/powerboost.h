// powerboost: the client half of EC's POWER BOOST (source build the source build's powerboost, "powerboost.on", TEST ONLY).
// Rows R51..R56 (docs/quest/q15t/modules/powerboost.md). A GOT-wrapper module: data-page swaps only, no code word written.
//
//   stage 1 (read-only): wrappers on CR15NetDamageableHealNode::Enter, CR15NetDamageableCS::Heal and
//     CR15NetHealBubbleCS::SetTargetInsideBubble that only record where heal amounts of 1,000,000 and more pass and what the
//     heal bubble's link call is given. Every call is passed on untouched.
//   stage 2 (swallow): a marker never reaches Heal; the node's queued heal (Heal's GLOB_DAT word) is caught, and a "used it"
//     marker is sent to the server as DamageFromClient(cs, handle, -amount, 0, 0).
//   stage 3 (cone): ResolveBody's JUMP_SLOT holds a naked arm64 thunk that acts on the one call UpdateActiveHealBubble makes
//     per target; a target outside the owner's +z cone gets a far stand-in body; link / unlink markers go out from the net tick.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

struct PhTarget {  // CPhTarget, 24 bytes (returned through x8 by CPhysicsCS::PrimaryBody)
  std::uint64_t w[3];
};

struct PowerboostFns {
  // CR15NetDamageableCS::DamageFromClient(cs, handle, float amount (s0), hit body (w2), symbol (x3))
  void (*send)(void* cs, std::uint64_t handle, float amount, std::uint32_t hit_body, std::uint64_t symbol);
  const std::uint8_t* (*resolve)(const PhTarget* target);                   // CPhTarget::ResolveBody (the real one)
  PhTarget (*primary)(const void* physics, std::uint32_t handle);            // CPhysicsCS::PrimaryBody(SCompactPoolHandle) const
};

// The three engine functions by exported name (dlsym on the loaded libr15.so), each required to be at its pinned address
// (base + ELF vaddr: DamageFromClient 0x213e80c, ResolveBody 0x19a27ac, PrimaryBody 0x1677d34). False when one is missing
// or elsewhere; nothing is written.
bool ResolvePowerboostFns(std::uintptr_t base, PowerboostFns* out);

enum PowerboostStage { kPowerboostOff = 0, kPowerboostRead = 1, kPowerboostSwallow = 2, kPowerboostCone = 3 };

// `stage`: see above (the source build's default is OFF; its QA stage is 1 = read-only).
// `net_tick`: true when the caller calls PowerboostNetTick() from a net context; without it stage 3 is refused (loudly) and
//   the module stays at stage 2.
// `fns`: null = ResolvePowerboostFns(base) (the production path); a test passes fakes.
// Every hook is logged (one structured line each); a refusal is ERROR/WARNING. All three stage-1 words or none: a failed
// later swap puts the earlier ones back. `status`: the first stage-1 failure (kOk when all three are in); `reason` names a
// degraded stage ("swallow_off", "cone_off") or an error token, "" when the stage asked for is fully on.
InstallResult InstallPowerboost(std::uintptr_t base, int stage, bool net_tick, const PowerboostFns* fns = nullptr);

// A TickFn (game_tick.h): records the CR15Game for the live-system check of the stage-3 sends. One atomic store per frame.
void PowerboostGameTick(std::uintptr_t game);

// Stage 3's link / unlink sends (source: source-build ec net tick = the game loop's window around UpdateBroadcaster). Cheap when idle.
void PowerboostNetTick();

struct PowerboostStats {
  std::uint32_t markers, swallowed, sent, held, links, cone_calls, cone_rejects, pending_sent, pending_dropped;
  int stage;        // the stage that is actually on (1..3), 0 when nothing is installed
  bool cone_armed;  // the thunk is in ResolveBody's slot and the cone is live
};
PowerboostStats GetPowerboostStats();

// test seams
void SetPowerboostClockForTest(std::uint64_t (*now_ms)());
void PowerboostSwallowForTest(bool on);  // the flag the queued-heal wrapper reads
void ResetPowerboostForTest();
// ConeAt: what the thunk calls for the one call it acts on
const std::uint8_t* PowerboostConeAtForTest(const PhTarget* target, const std::uint8_t* frame, std::uintptr_t dcs);

}  // namespace q15t
