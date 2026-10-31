// pfxtev (issue #1642): the six event-effect ids (GPU events that spawn child effects) may be created by the particle renderer.
// Default ON; kill file pfxtev.off in the q15t data directory (module_switch.h) turns the event ids off.
//
// This is the decision only. The renderer that creates effects is pfx (issue #1631), which is not in this tree yet, so nothing
// calls PfxTevAllows until it lands; until then the switch has no effect on a headset. Source of the id list: the particle
// renderer's event tier (the upstream q15t reference, read as text). Details: docs/quest/q15t/modules/pfxtev.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace q15t {

// The event-effect ids, as symbol hashes of the pack's effects.
inline constexpr std::uint64_t kPfxTevIds[] = {
    0x3db472928b4d7c45ull, 0x75db6596e0af110dull, 0x76db6596e0af110dull,
    0x83976b2ec1de4a57ull, 0xb9e607923cff12f3ull, 0xbae607923cff12f3ull,
};
inline constexpr std::size_t kPfxTevIdCount = sizeof kPfxTevIds / sizeof kPfxTevIds[0];

// Reads the kill file once and logs the state (one INFO line). Until it runs the switch is on.
void InitPfxTev();

// True when the renderer may create `id`. An id that is not one of kPfxTevIds is not this switch's: true. An event id is true
// while the switch is on and false while it is off. Each event-id decision is one INFO line (WARNING once past the cap).
bool PfxTevAllows(std::uint64_t id);

struct PfxTevStats {
  std::uint32_t allowed;   // event-id decisions that allowed the create
  std::uint32_t refused;   // event-id decisions that refused it
};
PfxTevStats GetPfxTevStats();

// Test seam: back to the state before InitPfxTev (on, counters zero).
void ResetPfxTevForTest();

}  // namespace q15t
