/* The autopilot's route file and clock (#1202, docs/adr/0025-quest-autopilot-route.md, "The route file"): pure, no thread,
 * no file and no Android header, so the host test (src/quest/tests/autopilot_route_test.cpp) drives every rule.
 *
 * Button and touch bits are VrApi_Input.h's (ovrButton, ovrTouch), the layout input_seam.h pins: the state a filter writes
 * through the host is the VrApi state, field for field.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_autopilot {

// ovrButton.
inline constexpr std::uint32_t kButtonA = 0x00000001;
inline constexpr std::uint32_t kButtonB = 0x00000002;
inline constexpr std::uint32_t kButtonRThumb = 0x00000004;
inline constexpr std::uint32_t kButtonX = 0x00000100;
inline constexpr std::uint32_t kButtonY = 0x00000200;
inline constexpr std::uint32_t kButtonLThumb = 0x00000400;
inline constexpr std::uint32_t kButtonEnter = 0x00100000;  // the menu button, on the left controller
inline constexpr std::uint32_t kButtonGripTrigger = 0x04000000;
inline constexpr std::uint32_t kButtonTrigger = 0x20000000;
inline constexpr std::uint32_t kButtonJoystick = 0x80000000;

// ovrTouch.
inline constexpr std::uint32_t kTouchA = 0x00000001;
inline constexpr std::uint32_t kTouchB = 0x00000002;
inline constexpr std::uint32_t kTouchX = 0x00000004;
inline constexpr std::uint32_t kTouchY = 0x00000008;
inline constexpr std::uint32_t kTouchJoystick = 0x00000020;
inline constexpr std::uint32_t kTouchIndexTrigger = 0x00000040;
inline constexpr std::uint32_t kTouchThumbUp = 0x00000100;
inline constexpr std::uint32_t kTouchIndexPointing = 0x00000200;
inline constexpr std::uint32_t kTouchLThumb = 0x00000400;
inline constexpr std::uint32_t kTouchRThumb = 0x00000800;

// The limits the ADR states.
inline constexpr std::size_t kMaxFileBytes = 64 * 1024;
inline constexpr std::size_t kMaxSteps = 256;
inline constexpr std::int64_t kNsPerSecond = 1000000000;
inline constexpr std::int64_t kMinStepNs = kNsPerSecond / 100;           // 0.01 s
inline constexpr std::int64_t kMaxStepNs = 600 * kNsPerSecond;           // 600 s
inline constexpr std::int64_t kMaxStartAfterNs = 600 * kNsPerSecond;     // 600 s
inline constexpr std::int64_t kMaxRouteNs = 3600 * kNsPerSecond;         // the steps times repeat, 3600 s
inline constexpr std::uint32_t kMaxRepeat = 1000;

enum Hand : std::uint32_t { kLeft = 0, kRight = 1 };

// One hand's whole state for a step: what the filter writes, derived bits included.
struct HandState {
  std::uint32_t buttons = 0;
  std::uint32_t touches = 0;
  float trigger = 0.0f;
  float grip = 0.0f;
  float stickX = 0.0f;
  float stickY = 0.0f;
};

struct Step {
  std::int64_t durationNs = 0;
  HandState hands[2];
};

struct Route {
  std::string name;
  std::int64_t startAfterNs = 0;
  std::uint32_t repeat = 1;
  std::vector<Step> steps;
  // Which hands any step names a control of. A hand the route never names is never written.
  bool drives[2] = {false, false};
  std::int64_t PassNs() const noexcept;
  std::int64_t PlannedNs() const noexcept { return PassNs() * static_cast<std::int64_t>(repeat); }
};

struct ParseError {
  std::size_t line = 0;  // 1-based; 0 for a file-level error (too large, empty)
  std::string reason;
};

// Parses a route file. False, with the line and the reason, on any error: a route is accepted whole or not at all.
bool ParseRoute(std::string_view text, Route* route, ParseError* error);

// The state a step with no controls gives a hand: nothing pressed or touched, so the header's untouched base state.
HandState RestState() noexcept;

// Where a route is at a time: before its first step, inside step `index` of pass `pass` (both 0-based), or done.
struct Position {
  enum Phase { kWaiting, kRunning, kDone } phase = kWaiting;
  std::uint32_t pass = 0;
  std::uint32_t index = 0;
};

// `sinceStartNs`: the time since the start point (the start delay counts from there). A boundary belongs to the step it
// opens, so a frame exactly on it is already in the next step, and the frame at the planned end is done.
Position Locate(const Route& route, std::int64_t sinceStartNs) noexcept;

}  // namespace nevr_autopilot
