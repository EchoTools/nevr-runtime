/* The controller-input seam (#1202, docs/adr/0025-quest-autopilot-route.md): the pure half; input_seam_hook.h is the hook.
 * No thunk and no Android header here, so the host test drives every rule directly (src/quest/tests/input_seam_test.cpp).
 *
 * What the game does (pinned libr15, build b243509c...; pinned VAs, the disassembly lists them 0x100000 higher): once per frame
 * NRadEngine::CVR::PollTracking (0x1a7142c) enumerates the input devices, keeps type 4 (`cmp w8, #0x4` at 0x1a718e4,
 * ovrControllerType_TrackedRemote), reads each one's capabilities (vrapi_GetInputDeviceCapabilities, call at 0x1a718fc),
 * takes the hand from the left-hand bit (0x1a71914: left 0, else 1) and calls
 * vrapi_GetCurrentInputState(ovr, deviceId, cvr + 0x300 + hand * 0x48) (call at 0x1a71928). That call is the only
 * caller of the import's PLT stub (callers of 0x1029410 in the disassembly).
 *
 * The layouts are the public Oculus Mobile SDK header VrApi_Input.h (API 1.1.15 and later). Two measurements on the
 * binary agree with it: the game's per-hand stride 0x48 is sizeof(ovrInputStateTrackedRemote), and its capabilities
 * buffer (sp+0x18 to sp+0x48) is sizeof(ovrInputTrackedRemoteCapabilities), whose ControllerCapabilities the game reads at
 * offset 8 (sp+0x20). The static_asserts below pin both, so a layout edit fails to compile.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "nevr/plugin_interface.h"

namespace nevr_quest::input_seam {

inline constexpr std::uint32_t kControllerTypeTrackedRemote = 4;  // ovrControllerType_TrackedRemote (1 << 2)
inline constexpr std::uint32_t kCapsLeftHand = 0x4;                // ovrControllerCaps_LeftHand
inline constexpr std::uint32_t kCapsRightHand = 0x8;               // ovrControllerCaps_RightHand

// ovrInputStateHeader.
struct VrapiInputStateHeader {
  std::uint32_t controllerType;
  double timeInSeconds;
};

// ovrInputStateTrackedRemote.
struct VrapiTrackedRemoteState {
  VrapiInputStateHeader header;
  std::uint32_t buttons;
  std::uint32_t trackpadStatus;
  float trackpadX;
  float trackpadY;
  std::uint8_t batteryPercentRemaining;
  std::uint8_t recenterCount;
  std::uint16_t reserved;
  float indexTrigger;
  float gripTrigger;
  std::uint32_t touches;
  std::uint32_t reserved5a;
  float joystickX;
  float joystickY;
  float joystickNoDeadZoneX;
  float joystickNoDeadZoneY;
};
static_assert(sizeof(VrapiTrackedRemoteState) == 0x48, "the game's per-hand stride (0x1a7191c: mov w8, #0x48)");
static_assert(offsetof(VrapiTrackedRemoteState, buttons) == 16, "VrApi_Input.h");
static_assert(offsetof(VrapiTrackedRemoteState, indexTrigger) == 36, "VrApi_Input.h");
static_assert(offsetof(VrapiTrackedRemoteState, gripTrigger) == 40, "VrApi_Input.h");
static_assert(offsetof(VrapiTrackedRemoteState, touches) == 44, "VrApi_Input.h");
static_assert(offsetof(VrapiTrackedRemoteState, joystickX) == 52, "VrApi_Input.h");
static_assert(offsetof(VrapiTrackedRemoteState, joystickNoDeadZoneX) == 60, "VrApi_Input.h");

// ovrInputTrackedRemoteCapabilities (its header is ovrInputCapabilityHeader { Type, DeviceID }).
struct VrapiTrackedRemoteCaps {
  std::uint32_t type;
  std::uint32_t deviceId;
  std::uint32_t controllerCapabilities;
  std::uint32_t buttonCapabilities;
  std::uint16_t trackpadMaxX;
  std::uint16_t trackpadMaxY;
  float trackpadSizeX;
  float trackpadSizeY;
  std::uint32_t hapticSamplesMax;
  std::uint32_t hapticSampleDurationMs;
  std::uint32_t touchCapabilities;
  std::uint32_t reserved4;
  std::uint32_t reserved5;
};
static_assert(sizeof(VrapiTrackedRemoteCaps) == 48, "the game's capabilities buffer, sp+0x18 to sp+0x48");
static_assert(offsetof(VrapiTrackedRemoteCaps, controllerCapabilities) == 8, "the game reads it at sp+0x20");

// ovrResult vrapi_GetInputDeviceCapabilities(ovrMobile* ovr, ovrInputCapabilityHeader* capsHeader).
using CapsFn = int (*)(void* ovr, VrapiTrackedRemoteCaps* caps);

// The hand a capabilities word names. Both bits (a two-handed device) or neither is UNKNOWN: the game would call it
// right (0x1a71914 takes "not left" as right); a filter is told it cannot be sure.
inline std::uint32_t HandFromCaps(std::uint32_t caps) noexcept {
  const bool left = (caps & kCapsLeftHand) != 0;
  const bool right = (caps & kCapsRightHand) != 0;
  if (left == right) return NEVR_QUEST_HAND_UNKNOWN;
  return left ? NEVR_QUEST_HAND_LEFT : NEVR_QUEST_HAND_RIGHT;
}

// The six fields a filter sees.
inline NevrQuestControllerState ReadState(const VrapiTrackedRemoteState& vr, std::uint32_t hand) noexcept {
  NevrQuestControllerState s{};
  s.hand = hand;
  s.buttons = vr.buttons;
  s.touches = vr.touches;
  s.index_trigger = vr.indexTrigger;
  s.grip_trigger = vr.gripTrigger;
  s.stick_x = vr.joystickX;
  s.stick_y = vr.joystickY;
  return s;
}

// Writes the filter's state back so that an unchanged state leaves every byte VrApi wrote. The five fields the filter
// sees one to one are stored as they are (an unchanged one is its own bytes again). The stick is the one field with two
// copies: only a changed stick sets the dead-zoned and the raw pair alike (the game may read either), so an unchanged
// stick keeps the raw pair VrApi wrote. The stick compares by bits, so a NaN the runtime wrote and the filter kept is
// "unchanged".
inline void WriteChanged(const NevrQuestControllerState& before, const NevrQuestControllerState& after,
                         VrapiTrackedRemoteState* vr) noexcept {
  const auto same = [](float a, float b) noexcept { return __builtin_memcmp(&a, &b, sizeof(float)) == 0; };
  vr->buttons = after.buttons;
  vr->touches = after.touches;
  vr->indexTrigger = after.index_trigger;
  vr->gripTrigger = after.grip_trigger;
  if (!same(after.stick_x, before.stick_x) || !same(after.stick_y, before.stick_y)) {
    vr->joystickX = after.stick_x;
    vr->joystickY = after.stick_y;
    vr->joystickNoDeadZoneX = after.stick_x;
    vr->joystickNoDeadZoneY = after.stick_y;
  }
}

// Which hand each device ID is, so the capabilities are asked once per device. Lock-free: an entry is one word
// (bit 63 valid, bits 32-33 the hand, bits 0-31 the ID). A device the cache has no room for is asked every time.
class HandCache {
 public:
  static constexpr std::size_t kEntries = 8;

  bool Find(std::uint32_t deviceId, std::uint32_t* hand) const noexcept {
    for (const auto& entry : entries_) {
      const std::uint64_t word = entry.load(std::memory_order_acquire);
      if ((word & kValid) != 0 && static_cast<std::uint32_t>(word) == deviceId) {
        *hand = static_cast<std::uint32_t>((word >> 32) & 0x3);
        return true;
      }
    }
    return false;
  }

  void Store(std::uint32_t deviceId, std::uint32_t hand) noexcept {
    const std::uint64_t word = kValid | (static_cast<std::uint64_t>(hand & 0x3) << 32) | deviceId;
    for (auto& entry : entries_) {
      std::uint64_t expected = 0;
      if (entry.compare_exchange_strong(expected, word, std::memory_order_acq_rel)) return;
      if (static_cast<std::uint32_t>(expected) == deviceId) return;  // another call stored it first
    }
  }

  void Clear() noexcept {
    for (auto& entry : entries_) entry.store(0, std::memory_order_release);
  }

 private:
  static constexpr std::uint64_t kValid = 1ULL << 63;
  std::atomic<std::uint64_t> entries_[kEntries] = {};
};

}  // namespace nevr_quest::input_seam
